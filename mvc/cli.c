/* Two-view MVC encoding loop for the stock x264 CLI.
 * GPL-2.0-or-later; x264 project contributors. No JMVC code is included. */
#include "cli.h"
#include "nal.h"
#include "joint_rc.h"
#include "shared_vbv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include "../common/osdep.h"
#include "../common/cpu.h"

typedef struct { x264_t *encoder; x264_param_t param; } mvc_view_t;

static int mvc_open( mvc_view_t *view, const x264_param_t *source, unsigned mask,
                     int dependent, const x264_mvc_cli_opts_t *opt, int width, int height, int view_threads,
                     const char *stats_path, int bitrate, const x264_mvc_joint_plan_t *plan, x264_mvc_shared_vbv_t *shared )
{
    view->param = *source;
    x264_param_t *p = &view->param;
    if( source->rc.b_stat_read || source->rc.b_stat_write )
    {
        p->rc.psz_stat_in = (char*)stats_path;
        p->rc.psz_stat_out = (char*)stats_path;
    }
    if( plan && plan->frames )
    {
        p->mvc_joint_qscale = plan->qscale[dependent];
        p->mvc_joint_frames = plan->frames;
    }
    if( bitrate > 0 )
    {
        p->rc.i_rc_method = X264_RC_ABR;
        p->rc.i_bitrate = bitrate;
    }
    if( dependent )
    {
        if( opt->dependent_bitrate && !bitrate )
        {
            p->rc.i_rc_method = X264_RC_ABR;
            p->rc.i_bitrate = opt->dependent_bitrate;
        }
        if( opt->dependent_vbv_maxrate )
            p->rc.i_vbv_max_bitrate = opt->dependent_vbv_maxrate;
        if( opt->dependent_vbv_bufsize )
            p->rc.i_vbv_buffer_size = opt->dependent_vbv_bufsize;
    }
    p->mvc_shared_vbv = shared;
    p->mvc_view_index = dependent;
    p->i_width = width;
    p->i_height = height;
    p->i_csp = X264_CSP_I420;
    p->i_sps_id = dependent ? 1 : 0;
    p->b_mvc_force_high = 1;
    p->b_mvc_inter_view = dependent && mask;
    p->b_mvc_inter_view_i = dependent && !!(mask & 1);
    p->b_mvc_inter_view_p = dependent && !!(mask & 2);
    p->b_mvc_inter_view_b = dependent && !!(mask & 4);
    p->b_full_recon = !dependent && !!mask;
    p->b_annexb = 1;
    p->b_repeat_headers = 0;
    p->b_open_gop = 0;
    /* Both encoders ingest pictures in display order. Their non-adaptive
     * B-picture decisions and closed GOP cadence match, while reconstructed
     * base references are published by PTS before dependent output needs them.
     * Slice threading retains deterministic reconstruction lifetime. */
    p->i_bframe = source->i_bframe;
    p->i_bframe_pyramid = X264_B_PYRAMID_NONE;
    /* Native x264 frame threads can expose a reconstructed base picture
     * before it is stable for the other encoder; this made AVC bytes vary
     * between runs and could corrupt startup pictures. Slice threading keeps
     * frame-to-frame dependencies ordered while still using multiple CPUs.
     * This applies to MVC only, never stock x264 2D encoding. */
    p->b_sliced_threads = 1;
    p->i_bframe_adaptive = X264_B_ADAPT_NONE;
    p->i_scenecut_threshold = 0;
    /* Preserve x264's native frame threading. Slice threading remains an
     * explicit stock CLI option, rather than a compulsory MVC fallback. */
    p->i_threads = view_threads;
    if( dependent && mask ) p->analyse.i_weighted_pred = X264_WEIGHTP_NONE;
    if( p->b_interlaced || p->b_fake_interlaced ||
        p->i_frame_reference > 8 || p->i_dpb_size > 8 ||
        x264_param_apply_profile( p, "high" ) )
        return -1;
    view->encoder = x264_encoder_open( p );
    return view->encoder ? 0 : -1;
}

static char *mvc_stats_name( const char *stem, const char *eye )
{
    size_t a = strlen( stem ), b = strlen( eye );
    if( a > SIZE_MAX-b-2 ) return NULL;
    char *name = malloc( a+b+2 );
    if( name ) snprintf( name, a+b+2, "%s.%s", stem, eye );
    return name;
}

static void mvc_copy_region( x264_picture_t *dst, const x264_picture_t *src,
                             int x, int y, int w, int h )
{
    for( int p = 0; p < 3; ++p )
    {
        int div = p ? 2 : 1;
        for( int row = 0; row < h/div; ++row )
            memcpy( dst->img.plane[p] + row * dst->img.i_stride[p],
                    src->img.plane[p] + (row + y/div) * src->img.i_stride[p] + x/div,
                    w/div );
    }
}
static int mvc_headers( mvc_view_t *view, FILE *out, int dependent, unsigned mask )
{
    x264_nal_t *nals = NULL;
    int count = 0, level = 40;
    if( x264_encoder_headers( view->encoder, &nals, &count ) < 0 ) return -1;
    for( int i = 0; i < count; ++i ) if( nals[i].i_type == 7 )
    {
        const uint8_t *p = nals[i].p_payload;
        int sc = p[2] == 1 ? 3 : 4;
        if( nals[i].i_payload > sc + 3 ) level = p[sc+3];
    }
    return x264_mvc_write_nals( out, nals, count, dependent, level, mask );
}
/* MVC picture-pipeline threading: x264 owns intra-view frame threading.
 * The dependent worker consumes immutable, bounded snapshots of base picture
 * reconstruction and base NALs; the producing encoder never exposes a frame
 * or NAL buffer to a concurrent consumer after re-entering x264_encoder_encode.
 * This also leaves each encoder's ratecontrol/statistics state thread-confined,
 * which is essential for a future independent-stats two-pass implementation. */
typedef struct mvc_job_t
{
    x264_picture_t right, reference;
    x264_picture_t base_meta;
    x264_nal_t *nals;
    uint8_t *nal_data;
    int nal_count, base_size, have_right, have_reference;
    struct mvc_job_t *next;
} mvc_job_t;

static void mvc_job_destroy( mvc_job_t *job )
{
    if( !job ) return;
    if( job->have_right ) x264_picture_clean( &job->right );
    if( job->have_reference ) x264_picture_clean( &job->reference );
    free( job->nals );
    free( job->nal_data );
    free( job );
}

static void mvc_picture_planes_copy( x264_picture_t *dst, const x264_picture_t *src,
                                     int width, int height )
{
    for( int plane = 0; plane < src->img.i_plane; plane++ )
    {
        /* Reconstructed x264 pictures are usually NV12 (two planes),
         * whereas CLI input is planar I420 (three planes). Retain the
         * source's colorspace; x264_frame_copy_picture handles both. */
        int w = plane && src->img.i_plane == 3 ? width / 2 : width;
        int h = plane ? height / 2 : height;
        for( int y = 0; y < h; y++ )
            memcpy( dst->img.plane[plane] + (size_t)y * dst->img.i_stride[plane],
                    src->img.plane[plane] + (size_t)y * src->img.i_stride[plane], w );
    }
}

static mvc_job_t *mvc_job_create( const mvc_view_t *base, int base_size,
                                  x264_nal_t *base_nals, int base_count,
                                  const x264_picture_t *recon, const x264_picture_t *right, int stats_only )
{
    int width = base->param.i_width, height = base->param.i_height;
    mvc_job_t *job = calloc( 1, sizeof(*job) );
    if( !job ) return NULL;
    job->base_size = base_size;
    job->base_meta.i_pts = recon->i_pts;
    job->base_meta.i_dts = recon->i_dts;
    job->base_meta.i_type = recon->i_type;
    job->base_meta.b_keyframe = recon->b_keyframe;
    if( right )
    {
        if( x264_picture_alloc( &job->right, X264_CSP_I420, width, height ) ) goto fail;
        job->have_right = 1;
        mvc_picture_planes_copy( &job->right, right, width, height );
        job->right.i_pts = right->i_pts;
        /* The dependent view must start exactly the same closed GOPs as the
         * independently decodable base. Without this, first-pass dependent
         * stats mark frame 0 and each keyint boundary as P, and pass 2
         * rewrites those pictures as IDR (visible corruption near the start).
         * Non-keyframes remain AUTO; the fixed-GOP scheduler synchronizes B-pictures. */
        job->right.i_type = recon->b_keyframe ? X264_TYPE_IDR : X264_TYPE_AUTO;
    }
    if( base_size && base->param.b_full_recon )
    {
        if( recon->img.i_plane < 2 || recon->img.i_plane > 3 ||
            !recon->img.plane[0] || !recon->img.plane[1] ||
            (recon->img.i_plane == 3 && !recon->img.plane[2]) ||
            x264_picture_alloc( &job->reference, recon->img.i_csp, width, height ) ) goto fail;
        job->have_reference = 1;
        mvc_picture_planes_copy( &job->reference, recon, width, height );
        job->reference.i_pts = recon->i_pts;
    }
    if( base_size && !stats_only )
    {
        if( base_count <= 0 ) goto fail;
        size_t length = 0;
        for( int i = 0; i < base_count; ++i )
        {
            if( base_nals[i].i_payload < 0 || length > SIZE_MAX - (size_t)base_nals[i].i_payload ) goto fail;
            length += base_nals[i].i_payload;
        }
        if( (size_t)base_count > SIZE_MAX / sizeof(*job->nals) ) goto fail;
        job->nals = malloc( (size_t)base_count * sizeof(*job->nals) );
        job->nal_data = malloc( length ? length : 1 );
        if( !job->nals || !job->nal_data ) goto fail;
        size_t offset = 0;
        for( int i = 0; i < base_count; i++ )
        {
            job->nals[i] = base_nals[i];
            memcpy( job->nal_data + offset, base_nals[i].p_payload, base_nals[i].i_payload );
            job->nals[i].p_payload = job->nal_data + offset;
            offset += base_nals[i].i_payload;
        }
        job->nal_count = base_count;
    }
    return job;
fail:
    mvc_job_destroy( job );
    return NULL;
}

/* Base NALs are queued in decode order; both input views run in display order. */
typedef struct {
    mvc_view_t *dependent;
    mvc_job_t *pending_head, *pending_tail;
    FILE *out, *depout;
    int stats_only;
    unsigned mask;
    int64_t written;
    x264_mvc_hrd_t hrd;
    int combined_hrd;
} mvc_pair_state_t;

static void mvc_pair_destroy( mvc_pair_state_t *s )
{
    while( s->pending_head )
    {
        mvc_job_t *job = s->pending_head;
        s->pending_head = job->next;
        mvc_job_destroy( job );
    }
    s->pending_tail = NULL;
}

/* Queue each base output in decode order, then feed the dependent encoder the
 * NEXT display-order input. The encoders have the same fixed B-frame schedule.
 * A PTS-specific base reconstruction is available when dependent encoding
 * consumes the corresponding picture in decode order. */
static int mvc_pair_queue_base( mvc_pair_state_t *s, mvc_job_t *job )
{
    if( !job ) return 0;
    if( job->have_reference && s->mask &&
        x264_encoder_mvc_reference( s->dependent->encoder, &job->reference ) )
    {
        mvc_job_destroy( job );
        fprintf( stderr, "x264 MVC: base reference handoff failed\n" );
        return -1;
    }
    job->next = NULL;
    if( s->pending_tail ) s->pending_tail->next = job;
    else s->pending_head = job;
    s->pending_tail = job;
    return 0;
}

static int mvc_pair_feed( mvc_pair_state_t *s, x264_picture_t *input )
{
    x264_nal_t *dep_nals = NULL;
    int dep_count = 0;
    x264_picture_t dep_meta = {0};
    int size = x264_encoder_encode( s->dependent->encoder, &dep_nals, &dep_count,
                                    input, &dep_meta );
    if( size < 0 ) return -1;
    if( !size ) return 0;
    mvc_job_t *base_job = s->pending_head;
    if( !base_job ) { fprintf(stderr,"x264 MVC: dependent emitted before base\n"); return -1; }
    if( base_job->base_meta.i_pts != dep_meta.i_pts ||
        base_job->base_meta.i_type != dep_meta.i_type ||
        base_job->base_meta.b_keyframe != dep_meta.b_keyframe )
    {
        fprintf( stderr, "x264 MVC: base/dependent picture order diverged (%" PRId64
                 " vs %" PRId64 "), types %d/%d, dts %" PRId64 "/%" PRId64 ", key=%d/%d\n",
                 base_job->base_meta.i_pts, dep_meta.i_pts,
                 base_job->base_meta.i_type, dep_meta.i_type,
                 base_job->base_meta.i_dts, dep_meta.i_dts,
                 base_job->base_meta.b_keyframe, dep_meta.b_keyframe );
        return -1;
    }
    if( !s->stats_only && s->combined_hrd &&
        x264_mvc_hrd_commit( &s->hrd, (size_t)base_job->base_size + (size_t)size ) )
    {
        fprintf(stderr,"x264 MVC [error]: combined elementary-stream CPB exceeded at pair %" PRId64
                       " (remaining %.0f bits); encoding aborted\n", s->written, s->hrd.fill_bits);
        return -1;
    }
    if( !s->stats_only &&
        (x264_mvc_write_nals( s->out, base_job->nals, base_job->nal_count, 0, 40, s->mask ) ||
         x264_mvc_write_nals( s->depout, dep_nals, dep_count, 1, 40, s->mask )) )
        return -1;
    s->pending_head = base_job->next;
    if( !s->pending_head ) s->pending_tail = NULL;
    mvc_job_destroy( base_job );
    ++s->written;
    return 0;
}

static int mvc_pair_drain( mvc_pair_state_t *s )
{
    while( x264_encoder_delayed_frames( s->dependent->encoder ) )
        if( mvc_pair_feed( s, NULL ) ) return -1;
    return s->pending_head ? -1 : 0;
}

#if HAVE_THREAD
/* Bounded FIFO: no unbounded picture allocations or movie-sized buffering. */
typedef struct
{
    mvc_job_t **queue;
    int capacity, count, head, tail, finished, error;
    mvc_view_t *dependent;
    FILE *out, *depout;
    unsigned mask;
    int stats_only;
    mvc_pair_state_t pair;
    x264_pthread_t thread;
    x264_pthread_mutex_t mutex;
    x264_pthread_cond_t have_job, have_room;
} mvc_pipeline_t;

static void *mvc_worker( void *opaque )
{
    mvc_pipeline_t *pipe = opaque;
    for( ;; )
    {
        x264_pthread_mutex_lock( &pipe->mutex );
        while( !pipe->count && !pipe->finished )
            x264_pthread_cond_wait( &pipe->have_job, &pipe->mutex );
        if( !pipe->count && pipe->finished )
        {
            x264_pthread_mutex_unlock( &pipe->mutex );
            break;
        }
        mvc_job_t *job = pipe->queue[pipe->head];
        pipe->queue[pipe->head] = NULL;
        pipe->head = (pipe->head + 1) % pipe->capacity;
        pipe->count--;
        x264_pthread_cond_broadcast( &pipe->have_room );
        x264_pthread_mutex_unlock( &pipe->mutex );
        int failed = mvc_pair_queue_base( &pipe->pair, job );
        if( failed )
        {
            x264_pthread_mutex_lock( &pipe->mutex );
            pipe->error = pipe->finished = 1;
            x264_pthread_cond_broadcast( &pipe->have_room );
            x264_pthread_mutex_unlock( &pipe->mutex );
            break;
        }
    }
    if( !pipe->error && mvc_pair_drain( &pipe->pair ) )
    {
        x264_pthread_mutex_lock( &pipe->mutex );
        pipe->error = 1;
        x264_pthread_mutex_unlock( &pipe->mutex );
    }
    return NULL;
}
static int mvc_pipeline_submit( mvc_pipeline_t *pipe, mvc_job_t *job )
{
    x264_pthread_mutex_lock( &pipe->mutex );
    while( pipe->count == pipe->capacity && !pipe->error )
        x264_pthread_cond_wait( &pipe->have_room, &pipe->mutex );
    if( pipe->error )
    {
        x264_pthread_mutex_unlock( &pipe->mutex );
        return -1;
    }
    pipe->queue[pipe->tail] = job;
    pipe->tail = (pipe->tail + 1) % pipe->capacity;
    pipe->count++;
    x264_pthread_cond_broadcast( &pipe->have_job );
    x264_pthread_mutex_unlock( &pipe->mutex );
    return 0;
}
static int mvc_pipeline_start( mvc_pipeline_t *pipe, mvc_view_t *dependent,
                               FILE *out, FILE *depout, unsigned mask, int depth, int stats_only, const x264_mvc_hrd_t *hrd )
{
    memset( pipe, 0, sizeof(*pipe) );
    pipe->capacity = depth;
    pipe->pair.dependent = dependent;
    pipe->pair.out = out;
    pipe->pair.depout = depout;
    pipe->pair.mask = mask;
    pipe->pair.stats_only = stats_only;
    if( hrd ) { pipe->pair.hrd = *hrd; pipe->pair.combined_hrd=1; }
    pipe->queue = calloc( depth, sizeof(*pipe->queue) );
    if( !pipe->queue ) return -1;
    if( x264_pthread_mutex_init( &pipe->mutex, NULL ) ) { free( pipe->queue ); pipe->queue = NULL; return -1; }
    if( x264_pthread_cond_init( &pipe->have_job, NULL ) ) goto fail_mutex;
    if( x264_pthread_cond_init( &pipe->have_room, NULL ) ) goto fail_job;
    if( x264_pthread_create( &pipe->thread, NULL, mvc_worker, pipe ) ) goto fail_room;
    return 0;
fail_room:
    x264_pthread_cond_destroy( &pipe->have_room );
fail_job:
    x264_pthread_cond_destroy( &pipe->have_job );
fail_mutex:
    x264_pthread_mutex_destroy( &pipe->mutex );
    free( pipe->queue ); pipe->queue = NULL;
    return -1;
}
static int mvc_pipeline_finish( mvc_pipeline_t *pipe )
{
    x264_pthread_mutex_lock( &pipe->mutex );
    pipe->finished = 1;
    x264_pthread_cond_broadcast( &pipe->have_job );
    x264_pthread_cond_broadcast( &pipe->have_room );
    x264_pthread_mutex_unlock( &pipe->mutex );
    x264_pthread_join( pipe->thread, NULL );
    int failure = pipe->error;
    mvc_pair_destroy( &pipe->pair );
    for( int i = 0; i < pipe->capacity; i++ ) mvc_job_destroy( pipe->queue[i] );
    free( pipe->queue ); pipe->queue = NULL;
    x264_pthread_cond_destroy( &pipe->have_room );
    x264_pthread_cond_destroy( &pipe->have_job );
    x264_pthread_mutex_destroy( &pipe->mutex );
    return failure ? -1 : 0;
}
#endif

int x264_mvc_cli_encode( const x264_param_t *param, const x264_mvc_cli_opts_t *opt,
                         x264_mvc_read_picture reader, void *opaque )
{
    int result = -1;
    int allocated = 0;
    int srcw = 0, srch = 0, w = 0, h = 0;
    int64_t submitted = 0, written = 0;
    int stats_only = 0, target_base = 0, target_dep = 0;
    char *base_stats = NULL, *dep_stats = NULL;
    x264_mvc_joint_plan_t plan = {0};
    x264_mvc_hrd_t aggregate = {0};
    x264_mvc_shared_vbv_t *shared = NULL;
    int combined_hrd = 0;
#if HAVE_THREAD
    mvc_pipeline_t pipeline = {0};
    int pipeline_started = 0;
#endif
    FILE *output = NULL, *dependent_output = NULL;
    mvc_view_t base = {0}, dependent = {0};
    x264_picture_t src = {0}, src_right = {0}, left = {0}, right = {0};
    x264_param_t settings;
    mvc_pair_state_t pair = {0};
    if( !param || !opt || !reader || !opt->enabled ) goto done;
    if( param->rc.b_stat_write && param->rc.b_stat_read ) goto done;
    if( !!opt->combined_vbv_maxrate != !!opt->combined_vbv_bufsize )
    {
        fprintf(stderr,"x264 MVC: combined VBV requires both --mvc-combined-vbv-* options\n");
        goto done;
    }
    stats_only = param->rc.b_stat_write;
    if( stats_only || param->rc.b_stat_read )
    {
        const char *stem = stats_only ? param->rc.psz_stat_out : param->rc.psz_stat_in;
        if( !stem || !*stem ) stem = "x264_2pass.log";
        base_stats = mvc_stats_name( stem, "base" );
        dep_stats = mvc_stats_name( stem, "dependent" );
        if( !base_stats || !dep_stats ) goto done;
        if( param->rc.i_rc_method != X264_RC_ABR )
        {
            fprintf( stderr, "x264 MVC: two-pass requires an ABR --bitrate target\n" );
            goto done;
        }
        if( param->rc.b_stat_read && !opt->no_joint_ratecontrol )
        {
            int configured_dep=opt->dependent_bitrate ? opt->dependent_bitrate : param->rc.i_bitrate;
            if( x264_mvc_joint_plan( &plan, base_stats, dep_stats, param->rc.i_bitrate,
                                     configured_dep, (double)param->i_fps_num/param->i_fps_den,
                                     opt->combined_vbv_maxrate, opt->combined_vbv_bufsize ) )
            {
                fprintf(stderr,"x264 MVC: invalid or mismatched first-pass RD statistics\n");
                goto done;
            }
            target_base=plan.bitrate[0];
            target_dep=plan.bitrate[1];
        }
    }
    srcw = param->i_width;
    srch = param->i_height;
    if( param->i_bitdepth != 8 || (param->i_csp & X264_CSP_MASK) != X264_CSP_I420 ||
        srcw <= 0 || srch <= 0 || (srcw & 1) || (srch & 1) || param->b_vfr_input ||
        !param->i_fps_den || !param->i_fps_num ) goto done;
    if( opt->dependent_filename && !strcmp( opt->output_filename, opt->dependent_filename ) ) goto done;
    w = opt->layout == 0 ? srcw/2 : srcw;
    h = opt->layout == 1 ? srch/2 : srch;
    if( (w & 1) || (h & 1) ) goto done;
    settings = *param;
    if( opt->layout == 2 && !opt->user_fps )
    {
        if( settings.i_fps_den > UINT32_MAX/2 ) goto done;
        settings.i_fps_den *= 2;
        settings.i_timebase_num = settings.i_fps_den;
        settings.i_timebase_den = settings.i_fps_num;
    }
    /* Separate per-eye and aggregate buffers share the same nominal fps. */
    if( opt->combined_vbv_maxrate && settings.i_fps_num && settings.i_fps_den &&
        !param->rc.b_stat_write )
    {
        if( x264_mvc_hrd_init( &aggregate, opt->combined_vbv_maxrate,
                     opt->combined_vbv_bufsize, (double)settings.i_fps_num/settings.i_fps_den ) ) goto done;
        combined_hrd=1;
    }
    if( opt->requested_frames ) settings.i_frame_total = opt->requested_frames;
    else if( opt->layout == 2 && settings.i_frame_total > 0 ) settings.i_frame_total /= 2;
    if( !stats_only && !opt->output_filename ) goto done;
    /* The two encoders retain their own native x264 VBV checks, but draw
     * from a single mutex-protected credit pool. No fixed buffer partition:
     * a view returns unused credit when its frame finishes, so its partner
     * and subsequent frames can use that capacity. */
    if( combined_hrd )
    {
        int base_peak = settings.rc.i_vbv_max_bitrate;
        int dep_peak = opt->dependent_vbv_maxrate ? opt->dependent_vbv_maxrate : base_peak;
        int base_buf = settings.rc.i_vbv_buffer_size;
        int dep_buf = opt->dependent_vbv_bufsize ? opt->dependent_vbv_bufsize : base_buf;
        /* The aggregate settings can be the only VBV options supplied.
         * In that case give each native controller the *full* upper bound;
         * shared reservations coordinate actual usage between the eyes. */
        if( base_peak <= 0 ) settings.rc.i_vbv_max_bitrate = base_peak = opt->combined_vbv_maxrate;
        if( base_buf <= 0 ) settings.rc.i_vbv_buffer_size = base_buf = opt->combined_vbv_bufsize;
        if( dep_peak <= 0 ) dep_peak = base_peak;
        if( dep_buf <= 0 ) dep_buf = base_buf;

        int rate_base = target_base ? target_base : settings.rc.i_bitrate;
        int rate_dep = target_dep ? target_dep :
            (opt->dependent_bitrate ? opt->dependent_bitrate : settings.rc.i_bitrate);
        if( settings.rc.i_rc_method == X264_RC_ABR &&
            ((int64_t)rate_base + rate_dep > opt->combined_vbv_maxrate) )
        {
            fprintf(stderr,"x264 MVC: requested sum of view targets exceeds aggregate VBV peak\n");
            goto done;
        }
        shared = x264_mvc_shared_vbv_create( opt->combined_vbv_maxrate,
                      opt->combined_vbv_bufsize,
                      (double)settings.i_fps_num/settings.i_fps_den );
        if( !shared ) goto done;
        if( plan.frames && x264_mvc_shared_vbv_set_forecast( shared, plan.future_headroom,
                                                              plan.frames ) )
        {
            fprintf(stderr,"x264 MVC: could not initialize predictive shared-VBV forecast\n");
            goto done;
        }
        fprintf(stderr,"x264 MVC: native x264 per-view VBV coordinated by dynamic shared "
                       "buffer (%d kb/s, %d kbit; frame-thread reservations)\n",
                opt->combined_vbv_maxrate, opt->combined_vbv_bufsize);
    }
    output = stats_only ? NULL : (!strcmp( opt->output_filename, "-" ) ? stdout : fopen( opt->output_filename, "wb" ));
    if( !stats_only && !output ) goto done;
    dependent_output = output;
    if( opt->dependent_filename && !stats_only )
    {
        dependent_output = !strcmp( opt->dependent_filename, "-" ) ? stdout :
            fopen( opt->dependent_filename, "wb" );
        if( !dependent_output || dependent_output == output ) goto done;
    }
    /* In MVC mode --threads is a *combined* budget, not per-eye. Reserve
     * at least one encoding worker for each view. A budget of one uses the
     * serial path to avoid two active encoder workers. Stock 2D is untouched. */
    int thread_budget = settings.i_threads;
    if( thread_budget == X264_THREADS_AUTO )
        thread_budget = x264_cpu_num_processors();
    if( thread_budget < 1 ) thread_budget = 1;
    int base_threads = thread_budget > 1 ? (thread_budget + 1) / 2 : 1;
    int dep_threads = thread_budget > 1 ? thread_budget / 2 : 1;
    fprintf( stderr, "x264 MVC: total frame/slice workers %d (base %d, dependent %d)\n",
             thread_budget, base_threads, dep_threads );
    if( mvc_open( &base, &settings, opt->prediction_mask, 0, opt, w, h, base_threads, base_stats, target_base, &plan, shared ) ||
        mvc_open( &dependent, &settings, opt->prediction_mask, 1, opt, w, h, dep_threads, dep_stats, target_dep, &plan, shared ) ) goto done;
    if( x264_picture_alloc( &src, X264_CSP_I420, srcw, srch ) ) goto done;
    allocated = 1;
    if( x264_picture_alloc( &src_right, X264_CSP_I420, srcw, srch ) ) goto done;
    allocated = 2;
    if( x264_picture_alloc( &left, X264_CSP_I420, w, h ) ) goto done;
    allocated = 3;
    if( x264_picture_alloc( &right, X264_CSP_I420, w, h ) ) goto done;
    allocated = 4;
    pair.dependent = &dependent;
    pair.out = output;
    pair.depout = dependent_output;
    pair.mask = opt->prediction_mask;
    pair.stats_only = stats_only;
    if(combined_hrd) { pair.hrd=aggregate; pair.combined_hrd=1; }
    if( !stats_only && (mvc_headers( &base, output, 0, opt->prediction_mask ) ||
        mvc_headers( &dependent, dependent_output, 1, opt->prediction_mask )) ) goto done;
#if HAVE_THREAD
    /* Until inter-view reference publication has a proper picture lifetime
     * barrier, never run base and dependent encoders simultaneously. Slice
     * threads still parallelize work *within* each view. */
    if( 0 && !opt->serial_mode && thread_budget > 1 )
    {
        if( mvc_pipeline_start( &pipeline, &dependent, output, dependent_output,
                                opt->prediction_mask, opt->pipeline_depth ? opt->pipeline_depth : 3, stats_only,
                                combined_hrd ? &aggregate : NULL ) ) goto done;
        pipeline_started = 1;
    }
#endif
    fprintf(stderr,"x264 MVC: synchronized display-order B-GOP pipeline (no speculative eye overlap; slice workers enabled)\n");
    /* Buffered raw right frames preserve display order while base output
     * is emitted in decode order.  Never allow the dependent encoder to
     * race ahead of its reconstructed base-view references. */
    while( !settings.i_frame_total || submitted < settings.i_frame_total )
    {
        int a = reader( opaque, &src, opt->layout == 2 ? 2 * submitted : submitted, 0 );
        if( a < 0 ) goto done;
        if( a > 0 ) break;
        if( opt->layout == 3 || opt->layout == 2 )
        {
            int b = reader( opaque, &src_right, opt->layout == 2 ? 2 * submitted + 1 : submitted,
                            opt->layout == 3 );
            if( b ) goto done;
        }
        if( opt->layout == 0 )
        {
            mvc_copy_region( &left, &src, 0, 0, w, h );
            mvc_copy_region( &right, &src, w, 0, w, h );
        }
        else if( opt->layout == 1 )
        {
            mvc_copy_region( &left, &src, 0, 0, w, h );
            mvc_copy_region( &right, &src, 0, h, w, h );
        }
        else
        {
            mvc_copy_region( &left, &src, 0, 0, w, h );
            mvc_copy_region( &right, &src_right, 0, 0, w, h );
        }
        left.i_pts = right.i_pts = submitted;
        x264_nal_t *nals = NULL;
        int count = 0;
        x264_picture_t out_base = {0};
        int size = x264_encoder_encode( base.encoder, &nals, &count, &left, &out_base );
        if( size < 0 ) goto done;
        if( size )
        {
            mvc_job_t *job = mvc_job_create( &base, size, nals, count, &out_base, NULL, stats_only );
            if( !job || mvc_pair_queue_base( &pair, job ) ) goto done;
        }
        /* Right eye receives its source frame in DISPLAY order, as does the
         * base encoder. Feeding it in base decode order corrupts B-GOPs. */
        if( mvc_pair_feed( &pair, &right ) ) goto done;
        submitted++;
    }
    while( x264_encoder_delayed_frames( base.encoder ) )
    {
        x264_nal_t *nals = NULL;
        int count = 0;
        x264_picture_t out_base = {0};
        int size = x264_encoder_encode( base.encoder, &nals, &count, NULL, &out_base );
        if( size < 0 ) goto done;
        if( size )
        {
            mvc_job_t *job = mvc_job_create( &base, size, nals, count, &out_base, NULL, stats_only );
            if( !job || mvc_pair_queue_base( &pair, job ) ) goto done;
        }
    }
#if HAVE_THREAD
    if( pipeline_started )
    {
        int pipeline_result = mvc_pipeline_finish( &pipeline );
        pipeline_started = 0;
        if( pipeline_result ) goto done;
        written = pipeline.pair.written;
    }
    else
#endif
    {
        if( mvc_pair_drain( &pair ) ) goto done;
        written = pair.written;
    }
    if( x264_encoder_delayed_frames( dependent.encoder ) || written != submitted ||
        (output && fflush( output )) || (dependent_output && dependent_output != output && fflush( dependent_output )) ) goto done;
    if( shared )
    {
        double minimum = 0;
        int inflight = 0;
        if( x264_mvc_shared_vbv_status( shared, &minimum, NULL, &inflight ) || inflight )
        {
            fprintf(stderr, "x264 MVC [error]: shared VBV has unfinished or oversized frame reservations\n");
            goto done;
        }
        fprintf(stderr,"x264 MVC: dynamic shared VBV completed (minimum available credit %.0f bits)\n",minimum);
    }
    fprintf( stderr, "x264 MVC: %" PRId64 " stereo pairs %s\n", written, stats_only ? "analyzed (no video written)" : "encoded" );
    result = 0;
done:
#if HAVE_THREAD
    if( pipeline_started ) mvc_pipeline_finish( &pipeline );
#endif
    mvc_pair_destroy( &pair );
    if( result ) fprintf( stderr, "x264 [error]: MVC encode failed; verify input layouts and constraints\n" );
    if( allocated >= 4 ) x264_picture_clean( &right );
    if( allocated >= 3 ) x264_picture_clean( &left );
    if( allocated >= 2 ) x264_picture_clean( &src_right );
    if( allocated >= 1 ) x264_picture_clean( &src );
    if( dependent.encoder ) x264_encoder_close( dependent.encoder );
    if( base.encoder ) x264_encoder_close( base.encoder );
    if( shared ) x264_mvc_shared_vbv_destroy( shared );
    if( dependent_output && dependent_output != stdout && dependent_output != output ) fclose( dependent_output );
    if( output && output != stdout ) fclose( output );
    free( base_stats );
    free( dep_stats );
    x264_mvc_joint_plan_clean(&plan);
    return result;
}
