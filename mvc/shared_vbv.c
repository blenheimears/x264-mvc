/* Copyright (C) 2026 the x264-MVC contributors. GPL-2.0-or-later.
 * Experimental MVC aggregate VBV, not normative MVC HRD signaling.
 * Native x264 row VBV controls each eye. This ledger accounts for the
 * combined access units in decode order, independent of worker completion. */
#include "shared_vbv.h"
#include <stdlib.h>
#include <math.h>
#include <limits.h>
#include <string.h>

typedef struct mvc_vbv_slot_t
{
    int64_t order;
    double grant[2], actual[2];
    unsigned started, finished;
    struct mvc_vbv_slot_t *next;
} mvc_vbv_slot_t;

struct x264_mvc_shared_vbv_t
{
    x264_pthread_mutex_t mutex;
    mvc_vbv_slot_t *head;  /* sorted by decode order */
    int64_t next_retire;
    double cap, refill, committed_fill, min_fill;
    int inflight, failed;
    x264_mvc_vbv_failure_t failure;
    double *future_headroom;
    int forecast_frames;
};

x264_mvc_shared_vbv_t *x264_mvc_shared_vbv_create( int rate, int buffer, double fps )
{
    if( rate <= 0 || buffer <= 0 || !isfinite(fps) || fps <= 0 ) return NULL;
    x264_mvc_shared_vbv_t *s = calloc( 1, sizeof(*s) );
    if( !s ) return NULL;
    s->cap = (double)buffer * 1000.;
    s->refill = (double)rate * 1000. / fps;
    s->committed_fill = s->min_fill = s->cap * .90;
    if( x264_pthread_mutex_init( &s->mutex, NULL ) )
    {
        free(s);
        return NULL;
    }
    return s;
}

void x264_mvc_shared_vbv_destroy( x264_mvc_shared_vbv_t *s )
{
    if( !s ) return;
    mvc_vbv_slot_t *p = s->head;
    while( p ) { mvc_vbv_slot_t *n = p->next; free(p); p = n; }
    free( s->future_headroom );
    x264_pthread_mutex_destroy( &s->mutex );
    free(s);
}

int x264_mvc_shared_vbv_set_forecast( x264_mvc_shared_vbv_t *s,
                                      const double *headroom_bits, int count )
{
    if( !s || !headroom_bits || count <= 0 ) return -1;
    if( (size_t)count > SIZE_MAX / sizeof(double) ) return -1;
    double *copy = malloc((size_t)count * sizeof(*copy));
    if( !copy ) return -1;
    for( int i = 0; i < count; ++i )
    {
        if( !isfinite(headroom_bits[i]) || headroom_bits[i] < 0 ||
            headroom_bits[i] > s->cap ) { free(copy); return -1; }
        copy[i]=headroom_bits[i];
    }
    x264_pthread_mutex_lock( &s->mutex );
    if( s->inflight || s->head || s->future_headroom )
    {
        x264_pthread_mutex_unlock( &s->mutex );
        free(copy);
        return -1;
    }
    s->future_headroom=copy;
    s->forecast_frames=count;
    x264_pthread_mutex_unlock( &s->mutex );
    return 0;
}

/* Project a buffer fill to the given decode-order pair.  Started but
 * unfinished eyes are charged their *estimated grants*; finished eyes are
 * charged their actual sizes. The projection is strictly advisory for QP
 * selection: a stale estimate must never be mistaken for a CPB overflow.
 *
 * The previous implementation refilled only up to min(last_seen[eye]) and
 * immediately debited completions. When one worker led by several frames,
 * its reservoir would reach zero despite all those pairs being independently
 * under the combined CPB limit. It then permanently poisoned the encoder.
 *
 * The ordered list lets us simulate the true decode timeline, skipping gaps
 * arithmetically rather than iterating over every frame of a long title. */
static double projected_fill( const x264_mvc_shared_vbv_t *s, int64_t order )
{
    double fill = s->committed_fill;
    int64_t next = s->next_retire;
    for( const mvc_vbv_slot_t *p = s->head; p && p->order <= order; p = p->next )
    {
        if( p->order > next )
        {
            /* Sparse reservations are possible when the encoders' frame
             * threads don't reach ratecontrol_start in lockstep. */
            const double gap = (double)(p->order - next) * s->refill;
            fill = fmin( s->cap, fill + fmin(s->cap, gap) );
        }
        fill = fmin( s->cap, fill + s->refill );
        for( int eye=0; eye<2; ++eye )
            if( p->started & (1u << eye) )
                fill -= (p->finished & (1u << eye)) ? p->actual[eye] : p->grant[eye];
        next = p->order + 1;
    }
    if( next <= order )
    {
        double gap = (double)(order - next + 1) * s->refill;
        fill = fmin( s->cap, fill + fmin(s->cap, gap) );
    }
    return fill;
}

/* Must be called with s->mutex held. Preserve the original failure rather
 * than overwriting the useful root cause with cascading subsequent errors. */
static void fail_locked( x264_mvc_shared_vbv_t *s, int64_t order,
                         const char *reason, double before,
                         double a, double b )
{
    if( s->failed ) return;
    s->failed = 1;
    s->failure.order = order;
    s->failure.reason = reason;
    s->failure.before = before;
    s->failure.refill = s->refill;
    s->failure.actual[0] = a;
    s->failure.actual[1] = b;
    s->failure.capacity = s->cap;
}

static mvc_vbv_slot_t **slot_position( mvc_vbv_slot_t **head, int64_t order )
{
    while( *head && (*head)->order < order ) head = &(*head)->next;
    return head;
}

int x264_mvc_shared_vbv_reserve( x264_mvc_shared_vbv_t *s, int64_t order,
                                  int eye, double estimate, double *allowed )
{
    if( !s || order < 0 || eye < 0 || eye > 1 || !allowed ||
        !isfinite(estimate) || estimate < 0 ) return -1;
    x264_pthread_mutex_lock( &s->mutex );
    if( s->failed || order < s->next_retire ) goto fail;
    mvc_vbv_slot_t **pos = slot_position( &s->head, order );
    mvc_vbv_slot_t *slot = *pos;
    if( !slot || slot->order != order )
    {
        slot = calloc( 1, sizeof(*slot) );
        if( !slot ) goto fail;
        slot->order = order;
        slot->next = *pos;
        *pos = slot;
    }
    if( slot->started & (1u << eye) ) goto fail;
    double free_bits = fmax( 0., projected_fill( s, order ) );
    double companion = (slot->started & (1u << (1-eye))) ? 0 :
                       fmin( free_bits * .20, fmax( 1024., s->refill * .10 ) );
    double future_guard = 0;
    if( s->future_headroom && order < s->forecast_frames )
        future_guard = fmin( free_bits * .35,
            fmax( 0., s->future_headroom[order] - .12*s->cap ) * .50 );
    double desired = fmax( 2048., estimate * 1.65 + 1024. );
    /* Even a fully occupied speculative reservation cannot be fatal: older
     * in-flight pictures may yet settle substantially below their grant.
     * A minimum grant drives native VBV/QP harder; actual capacity is
     * checked only after the pair is fully encoded in decode order. */
    double grant = fmax( 1024., fmin( desired,
                                   fmax( 0., free_bits - companion - future_guard ) ) );
    slot->started |= 1u << eye;
    slot->grant[eye] = grant;
    s->inflight++;
    *allowed = grant;
    x264_pthread_mutex_unlock( &s->mutex );
    return 0;
fail:
    fail_locked( s, order, "invalid-order-or-reservation", s->committed_fill, 0., 0. );
    x264_pthread_mutex_unlock( &s->mutex );
    return -1;
}

int x264_mvc_shared_vbv_settle( x264_mvc_shared_vbv_t *s, int64_t order,
                                 int eye, double actual )
{
    if( !s || order < 0 || eye < 0 || eye > 1 || !isfinite(actual) || actual < 0 ) return -1;
    x264_pthread_mutex_lock( &s->mutex );
    mvc_vbv_slot_t **pos = slot_position( &s->head, order );
    if( s->failed || !*pos || (*pos)->order != order ||
        !( (*pos)->started & (1u << eye) ) || ((*pos)->finished & (1u << eye)) )
        fail_locked( s, order, "invalid-order-or-settlement", s->committed_fill, 0., 0. );
    else
    {
        mvc_vbv_slot_t *p = *pos;
        p->finished |= 1u << eye;
        p->actual[eye] = actual;
        --s->inflight;
        /* One picture larger than the entire CPB is irremediable. Other
         * overruns must wait for the same-time eye and earlier pairs. */
        if( actual > s->cap + .5 )
            fail_locked( s, order, "single-eye-exceeds-capacity", s->committed_fill,
                         p->actual[0], p->actual[1] );
        while( !s->failed && s->head && s->head->order == s->next_retire &&
               s->head->finished == 3 )
        {
            mvc_vbv_slot_t *done = s->head;
            double before = s->committed_fill;
            s->committed_fill = fmin( s->cap, before + s->refill ) -
                                done->actual[0] - done->actual[1];
            s->min_fill = fmin( s->min_fill, s->committed_fill );
            if( s->committed_fill < -.5 )
                fail_locked( s, done->order, "actual-cpb-underflow", before,
                             done->actual[0], done->actual[1] );
            s->head = done->next;
            free( done );
            ++s->next_retire;
        }
    }
    int err = s->failed ? -1 : 0;
    x264_pthread_mutex_unlock( &s->mutex );
    return err;
}

int x264_mvc_shared_vbv_status( x264_mvc_shared_vbv_t *s, double *minfill,
                                 double *available, int *inflight )
{
    if( !s ) return -1;
    x264_pthread_mutex_lock( &s->mutex );
    if( minfill ) *minfill = s->min_fill;
    if( available )
    {
        const mvc_vbv_slot_t *tail = s->head;
        while( tail && tail->next ) tail = tail->next;
        *available = tail ? projected_fill( s, tail->order ) : s->committed_fill;
    }
    if( inflight ) *inflight = s->inflight;
    int err = s->failed ? -1 : 0;
    x264_pthread_mutex_unlock( &s->mutex );
    return err;
}

int x264_mvc_shared_vbv_failure( x264_mvc_shared_vbv_t *s,
                                 x264_mvc_vbv_failure_t *out )
{
    if( !s || !out ) return -1;
    x264_pthread_mutex_lock( &s->mutex );
    int err = !s->failed;
    if( !err ) *out = s->failure;
    x264_pthread_mutex_unlock( &s->mutex );
    return err ? -1 : 0;
}
