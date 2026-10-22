/* GPL-2.0-or-later. Shared MVC VBV escrow and concurrency regressions. */
#include "mvc/shared_vbv.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#if HAVE_POSIXTHREAD
#include <pthread.h>
#endif

static int check( int ok, const char *what )
{
    if( !ok ) fprintf(stderr,"shared-VBV test failed: %s\n",what);
    return !ok;
}

#if HAVE_POSIXTHREAD
typedef struct { x264_mvc_shared_vbv_t *s; int eye, error; } worker_t;
static void *run( void *data )
{
    worker_t *w=data;
    for(int n=0;n<100;n++)
    {
        double grant=0;
        /* Each worker processes its own eye in frame order, but the two
         * workers can progress independently, as in MVC frame threading. */
        if(x264_mvc_shared_vbv_reserve(w->s,n,w->eye,4000,&grant) ||
           grant<4096 || x264_mvc_shared_vbv_settle(w->s,n,w->eye,2000))
        { w->error=1; break; }
    }
    return NULL;
}
#endif

int main(void)
{
    double grant=0, before=0, after=0, minfill=0;
    int outstanding=-1, bad=0;
    x264_mvc_shared_vbv_t *s=x264_mvc_shared_vbv_create(1000,200,25.);
    bad |= check(s != NULL,"initialize");
    if(!s) return 1;
    bad |= check(!x264_mvc_shared_vbv_reserve(s,0,0,40000,&grant),"base reservation");
    bad |= check(!x264_mvc_shared_vbv_status(s,NULL,&before,&outstanding) && outstanding==1,
                 "inflight accounting");
    bad |= check(!x264_mvc_shared_vbv_settle(s,0,0,1000),"base credit return");
    bad |= check(!x264_mvc_shared_vbv_status(s,&minfill,&after,&outstanding) &&
                 outstanding==0 && after>before,"return unused credit for dependent view");
    bad |= check(!x264_mvc_shared_vbv_reserve(s,0,1,70000,&grant),"dependent borrowing");
    bad |= check(!x264_mvc_shared_vbv_settle(s,0,1,5000),"dependent settlement");
    bad |= check(!x264_mvc_shared_vbv_status(s,NULL,NULL,&outstanding) && outstanding==0,
                 "pair retirement");
    x264_mvc_shared_vbv_destroy(s);
#if HAVE_POSIXTHREAD
    s=x264_mvc_shared_vbv_create(200000,50000,24.);
    bad |= check(s!=NULL,"parallel initialize");
    if(s)
    {
        pthread_t t[2]; worker_t w[2]={{s,0,0},{s,1,0}};
        for(int eye=0;eye<2;eye++)
            if(pthread_create(&t[eye],NULL,run,&w[eye])) return 1;
        for(int eye=0;eye<2;eye++) pthread_join(t[eye],NULL);
        bad |= check(!w[0].error && !w[1].error,"concurrent reservations/settlements");
        bad |= check(!x264_mvc_shared_vbv_status(s,NULL,NULL,&outstanding) && outstanding==0,
                     "concurrent retirement");
        x264_mvc_shared_vbv_destroy(s);
    }
#endif
    /* TEST17: a 2-pass prediction can be low without the combined CPB being
     * overdrawn.  The shared controller must recover from that discrepancy. */
    s=x264_mvc_shared_vbv_create(1000,100,25.);
    bad |= check(s!=NULL,"estimate recovery initialize");
    if(s)
    {
        double extra=10000, left=0;
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,0,1000,&grant),"underestimated reserve");
        bad |= check(!x264_mvc_shared_vbv_settle(s,0,0,grant+extra),
                     "borrow unreserved shared credit for underestimated frame");
        bad |= check(!x264_mvc_shared_vbv_status(s,&minfill,&left,&outstanding) &&
                     outstanding==0 && left>=0,
                     "recovery keeps combined buffer solvent");
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,1,1000,&grant),"second eye after borrowing");
        bad |= check(!x264_mvc_shared_vbv_settle(s,0,1,1000),"second eye settlement");
        x264_mvc_shared_vbv_destroy(s);
    }
    s=x264_mvc_shared_vbv_create(1000,100,25.);
    bad |= check(s!=NULL,"out-of-order pair recovery initialize");
    if(s)
    {
        double base_grant=0, dep_grant=0;
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,0,30000,&base_grant),
                     "pair base reserve");
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,1,30000,&dep_grant),
                     "pair dependent reserve");
        /* One eye overruns enough to temporarily exhaust the free pool;
         * its companion under-runs, so the aggregate pair still fits. */
        double avail=0;
        bad |= check(!x264_mvc_shared_vbv_status(s,NULL,&avail,NULL),"read free shared credit");
        double excess=avail+2000;
        bad |= check(!x264_mvc_shared_vbv_settle(s,0,0,base_grant+excess),
                     "first settlement may temporarily owe partner credit");
        bad |= check(!x264_mvc_shared_vbv_settle(s,0,1,dep_grant-4000),
                     "companion credit resolves shared overrun");
        bad |= check(!x264_mvc_shared_vbv_status(s,NULL,&avail,&outstanding) &&
                     avail>=0 && outstanding==0,"paired accounting remains solvent");
        x264_mvc_shared_vbv_destroy(s);
    }
    s=x264_mvc_shared_vbv_create(1000,100,25.);
    bad |= check(s!=NULL,"true overflow initialize");
    if(s)
    {
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,0,1000,&grant),"overflow reserve");
        bad |= check(x264_mvc_shared_vbv_settle(s,0,0,grant+100000)!=0,
                     "reject truly exhausted aggregate capacity");
        x264_mvc_shared_vbv_destroy(s);
    }
    /* A second-pass forecast constrains a pre-burst grant ahead of time.
     * No source frames, replay buffer, or extra input pass is involved. */
    s=x264_mvc_shared_vbv_create(1000,200,25.);
    bad |= check(s!=NULL,"forecast initialize");
    if(s)
    {
        double forecast[] = {110000, 125000, 0};
        double capped=0;
        bad |= check(!x264_mvc_shared_vbv_set_forecast(s,forecast,3),"install forecast");
        bad |= check(x264_mvc_shared_vbv_set_forecast(s,forecast,3)!=0,
                     "reject replacing in-flight forecast");
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,0,120000,&capped),
                     "reserve with forecast");
        bad |= check(capped < 175000,"lookahead reduces pre-burst reservation");
        bad |= check(!x264_mvc_shared_vbv_settle(s,0,0,15000),"forecast frame settle");
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,1,12000,&grant),
                     "forecast companion reserve");
        bad |= check(!x264_mvc_shared_vbv_settle(s,0,1,5000),"forecast companion settle");
        x264_mvc_shared_vbv_destroy(s);
    }
    /* Frame threading is not lockstep: base pictures can complete far ahead
     * of dependent pictures. Each finished pair is cheap enough for the CPB,
     * so a 256-picture skew must never exhaust speculative reservations. */
    s=x264_mvc_shared_vbv_create(1000,200,25.);
    bad |= check(s!=NULL,"skew initialize");
    if(s)
    {
        for(int n=0; n<256 && !bad; ++n)
        {
            bad |= check(!x264_mvc_shared_vbv_reserve(s,n,0,90000,&grant),
                         "base must not run out of speculative grants");
            bad |= check(!x264_mvc_shared_vbv_settle(s,n,0,5000),
                         "skewed base settlement");
        }
        for(int n=0; n<256 && !bad; ++n)
        {
            bad |= check(!x264_mvc_shared_vbv_reserve(s,n,1,90000,&grant),
                         "dependent can catch up with base");
            bad |= check(!x264_mvc_shared_vbv_settle(s,n,1,5000),
                         "catch-up settlement retires in decode order");
        }
        bad |= check(!x264_mvc_shared_vbv_status(s,&minfill,NULL,&outstanding) &&
                     outstanding==0 && minfill>=0,"skewed pair accounting solvent");
        x264_mvc_shared_vbv_destroy(s);
    }
    /* A later picture pair may settle first.  It must not consume credit
     * ahead of an earlier picture pair, or trigger a false overflow. */
    s=x264_mvc_shared_vbv_create(1000,100,25.);
    bad |= check(s!=NULL,"out-of-order initialize");
    if(s)
    {
        for(int n=0;n<3;n++) for(int eye=0;eye<2;eye++)
            bad |= check(!x264_mvc_shared_vbv_reserve(s,n,eye,60000,&grant),
                         "out-of-order reservations");
        for(int n=2;n>=0;n--) for(int eye=1;eye>=0;eye--)
            bad |= check(!x264_mvc_shared_vbv_settle(s,n,eye,20000),
                         "out-of-order settlements");
        bad |= check(!x264_mvc_shared_vbv_status(s,&minfill,NULL,&outstanding) &&
                     outstanding==0 && minfill>=0,"decode-order commit");
        x264_mvc_shared_vbv_destroy(s);
    }
    /* More than 96,000 pairs (the frame number in the field report) cannot
     * create cumulative synthetic debt when each actual pair fits CPB. */
    s=x264_mvc_shared_vbv_create(60000,45000,24000./1001.);
    bad |= check(s!=NULL,"long-title initialize");
    if(s)
    {
        for(int n=0;n<96500 && !bad;n++)
        {
            for(int eye=0;eye<2;eye++)
            {
                bad |= check(!x264_mvc_shared_vbv_reserve(s,n,eye,3000000,&grant),
                             "long-title reservation");
                bad |= check(!x264_mvc_shared_vbv_settle(s,n,eye,600000),
                             "long-title settlement");
            }
        }
        bad |= check(!x264_mvc_shared_vbv_status(s,&minfill,NULL,&outstanding) &&
                     outstanding==0 && minfill>=0,"long-title remains solvent");
        x264_mvc_shared_vbv_destroy(s);
    }
    /* Actual decoded pair sizes, not grant estimates, must still catch a
     * genuine combined CPB overflow. */
    s=x264_mvc_shared_vbv_create(1000,100,25.);
    bad |= check(s!=NULL,"aggregate overflow initialize");
    if(s)
    {
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,0,20000,&grant),"overflow base reserve");
        bad |= check(!x264_mvc_shared_vbv_reserve(s,0,1,20000,&grant),"overflow dep reserve");
        bad |= check(!x264_mvc_shared_vbv_settle(s,0,0,75000),"overflow base settle");
        bad |= check(x264_mvc_shared_vbv_settle(s,0,1,40000)!=0,
                     "reject actual aggregate overflow at pair completion");
        x264_mvc_vbv_failure_t failure={0};
        bad |= check(!x264_mvc_shared_vbv_failure(s,&failure) &&
                     failure.order==0 &&
                     !strcmp(failure.reason,"actual-cpb-underflow") &&
                     failure.actual[0]==75000 && failure.actual[1]==40000 &&
                     failure.capacity==100000 && failure.refill==40000,
                     "error reports first actual CPB underflow with both view sizes");
        x264_mvc_shared_vbv_destroy(s);
    }
    if(!bad) puts("MVC shared VBV tests passed");
    return !!bad;
}
