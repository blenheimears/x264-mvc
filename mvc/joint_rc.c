/* GPL-2.0-or-later. Two-view first-pass RD proxy and aggregate CPB accounting.
 * The RD proxy is deliberately conservative: x264's native two-pass controller
 * and per-eye VBV still make the final quantizer and row-level decisions.
 * This is not a standard-conformance HRD timing/SEI generator. */
#include "joint_rc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>

typedef struct { double complexity, bits; int out, seen; } frame_cost_t;
typedef struct { frame_cost_t *a; int count; } costs_t;
static void costs_clean( costs_t *c ) { free(c->a); c->a=NULL; c->count=0; }
static int read_costs( const char *path, costs_t *c )
{
    FILE *f = fopen(path,"rb");
    if( !f ) return -1;
    /* x264 writes a single header line followed by semicolon-delimited records.
     * Read each line incrementally; reject duplicate/non-contiguous indices. */
    char line[16384];
    int capacity=0, bad=0, header=1;
    while( fgets(line,sizeof(line),f) )
    {
        if( !strchr(line,'\n') && !feof(f) ) { bad=1; break; }
        if( header ) { header=0; continue; }
        char *in=strstr(line,"in:"), *out=strstr(line,"out:"), *q=strstr(line," q:");
        char *tex=strstr(line," tex:"), *mv=strstr(line," mv:"), *misc=strstr(line," misc:");
        if( !in || !out || !q || !tex || !mv ) continue;
        int idx=-1, decode=-1, t=-1, m=-1, overhead=0;
        double qp=-1;
        if( sscanf(in,"in:%d",&idx)!=1 || sscanf(out,"out:%d",&decode)!=1 ||
            sscanf(q," q:%lf",&qp)!=1 || sscanf(tex," tex:%d",&t)!=1 ||
            sscanf(mv," mv:%d",&m)!=1 || (misc && sscanf(misc," misc:%d",&overhead)!=1) ||
            idx<0 || idx>10000000 || decode<0 || decode>10000000 ||
            t<0 || m<0 || overhead<0 || !isfinite(qp) || qp<0 || qp>69 ) { bad=1; break; }
        if( idx>=capacity )
        {
            int next=capacity?capacity:256;
            while(next<=idx && next<=10000000/2) next*=2;
            if( next<=idx ) { bad=1; break; }
            frame_cost_t *tmp=realloc(c->a,(size_t)next*sizeof(*tmp));
            if(!tmp) { bad=1; break; }
            c->a=tmp;
            memset(c->a+capacity,0,(size_t)(next-capacity)*sizeof(*tmp));
            capacity=next;
        }
        if(c->a[idx].seen) { bad=1; break; }
        c->a[idx].seen=1;
        c->a[idx].out=decode;
        c->a[idx].bits=(double)t+m+overhead;
        /* Normalize approximate residual complexity to a common QP. */
        c->a[idx].complexity=fmax(100.0, ((double)t+m+200.)*pow(2.0,(qp-26.)/12.));
        if( !isfinite(c->a[idx].complexity) ) { bad=1; break; }
        if(idx>=c->count) c->count=idx+1;
    }
    if(ferror(f)) bad=1;
    fclose(f);
    if( !c->count || bad ) return -1;
    for(int i=0;i<c->count;i++) if(!c->a[i].seen) return -1;
    return 0;
}

void x264_mvc_joint_plan_clean( x264_mvc_joint_plan_t *p )
{
    free(p->qscale[0]); free(p->qscale[1]);
    free(p->future_headroom);
    memset(p,0,sizeof(*p));
}

/* Protect capacity before a predicted burst rather than discovering exhaustion
 * when the burst arrives.  The forecast is in decode order (the order in
 * which x264's CPB consumes bits), not source presentation order.  It uses
 * only first-pass scalar costs: no compressed pictures are retained.  This
 * deliberately guides, rather than replaces, x264's native 2pass + VBV.
 * A finite horizon prevents a distant scene cut from over-penalizing every
 * frame before it.  The remaining safety floor absorbs estimation error. */
int x264_mvc_predictive_limit( double *bits, double *headroom, int count,
                                double refill_bits, double capacity_bits )
{
    if( !bits || !headroom || count <= 0 || !isfinite(refill_bits) ||
        !isfinite(capacity_bits) || refill_bits <= 0 || capacity_bits <= 0 )
        return -1;
    for( int i = 0; i < count; ++i )
        if( !isfinite(bits[i]) || bits[i] < 0 ) return -1;
    double fill = .80 * capacity_bits;
    const double floor_bits = .06 * capacity_bits;
    for( int i = 0; i < count; ++i )
    {
        double future_deficit = 0, accumulated = 0;
        int end = count-i-1 < 12 ? count : i+13;
        for( int j = i+1; j < end; ++j )
        {
            accumulated += bits[j] - refill_bits;
            future_deficit = fmax( future_deficit, accumulated );
        }
        /* Do not reserve the entire CPB: even the present AU needs bits. */
        double hold = fmin( .65 * capacity_bits, floor_bits + future_deficit );
        fill = fmin( capacity_bits, fill + refill_bits );
        double allowance = fmax( 256., fill - hold );
        bits[i] = fmin( bits[i], allowance );
        fill -= bits[i];
        if( fill < -1. ) return -1;
        headroom[i] = hold;
    }
    return 0;
}

/* Allocate a bounded per-access-unit budget while minimizing the convex proxy
 *  sum_view w(view, frame) / bits(view, frame).
 * Its closed form is bits proportional to sqrt(w). The two shares are allowed
 * to vary per picture pair; limits protect the dependent eye from consuming
 * excess rate on highly correlated images. The remainder stays available to
 * the more demanding base image and future frames.
 *
 * Hard bitstream compliance is checked separately at output: real x264 sizes
 * can differ from this first-pass prediction, especially at scene cuts. */
int x264_mvc_joint_plan( x264_mvc_joint_plan_t *plan, const char *bp, const char *dp,
                         int requested_base, int requested_dep, double fps,
                         int peak_kbps, int buffer_kbit )
{
    costs_t c[2]={{0},{0}};
    int result=-1;
    double *target[2]={NULL,NULL}, *weight[2]={NULL,NULL};
    int *by_out=NULL;
    double *paired_bits=NULL;
    if( !plan || requested_base<=0 || requested_dep<=0 || fps<=0 ||
        !isfinite(fps) || read_costs(bp,&c[0]) || read_costs(dp,&c[1]) ||
        c[0].count!=c[1].count ) goto done;
    const int n=c[0].count;
    for(int v=0;v<2;v++)
    {
        target[v]=calloc(n,sizeof(double));
        weight[v]=calloc(n,sizeof(double));
        plan->qscale[v]=calloc(n,sizeof(float));
        if(!target[v] || !weight[v] || !plan->qscale[v]) goto done;
    }
    by_out=malloc((size_t)n*sizeof(*by_out));
    if(!by_out) goto done;
    paired_bits=calloc(n,sizeof(*paired_bits));
    plan->future_headroom=calloc(n,sizeof(*plan->future_headroom));
    if(!paired_bits || !plan->future_headroom) goto done;
    for(int i=0;i<n;i++) by_out[i]=-1;
    double sums[2]={0,0}, weight_total=0;
    for(int i=0;i<n;i++)
    {
        if( c[0].a[i].out>=n || c[1].a[i].out != c[0].a[i].out ||
            by_out[c[0].a[i].out]!=-1 ) goto done;
        by_out[c[0].a[i].out]=i;
        for(int v=0;v<2;v++)
        {
            /* Smooth costs locally to avoid violent eye-switching at a cut. */
            double cost=c[v].a[i].complexity;
            if(i>0) cost=.8*cost+.2*c[v].a[i-1].complexity;
            weight[v][i]=sqrt(fmax(1.,cost));
            sums[v]+=weight[v][i];
            weight_total+=weight[v][i];
        }
    }
    const double total=(requested_base+(double)requested_dep)*1000.*n/fps;
    const double dep_budget=requested_dep*1000.*n/fps;
    /* A picture-pair has a shared budget based on both views' complexity;
     * distributing each eye's entire clip in isolation would never change
     * the relative allocation of the two eyes at a given instant. */
    double pair_weight_sum=0;
    for(int i=0;i<n;i++)
        pair_weight_sum+=sqrt(c[0].a[i].complexity+c[1].a[i].complexity);
    double unscaled_dep=0;
    for(int i=0;i<n;i++)
    {
        double pair_budget=total*sqrt(c[0].a[i].complexity+c[1].a[i].complexity)/pair_weight_sum;
        /* RD split responds to same-time cross-eye novelty, not only each
         * view's temporally varying source complexity. */
        double novelty=c[1].a[i].complexity/(c[0].a[i].complexity+c[1].a[i].complexity);
        double dep_fraction=fmin(.49,fmax(.025,novelty));
        target[1][i]=pair_budget*dep_fraction;
        target[0][i]=pair_budget-target[1][i];
        unscaled_dep+=target[1][i];
    }
    /* Respect the user's dependent target as an upper limit; all other
     * bits remain available to improve the independent AVC base view. */
    if(unscaled_dep>dep_budget)
    {
        double scale=dep_budget/unscaled_dep;
        for(int i=0;i<n;i++)
        {
            double saved=target[1][i]*(1-scale);
            target[1][i]*=scale;
            target[0][i]+=saved;
        }
    }
    /* Joint lookahead: convert the planned pair budgets to decode order,
     * plan future CPB headroom, and scale each eye back proportionately.
     * The native per-eye x264 VBV controller remains the final authority. */
    for( int order=0; order<n; ++order )
    {
        int i=by_out[order];
        if( i<0 ) goto done;
        paired_bits[order]=target[0][i]+target[1][i];
    }
    if( peak_kbps>0 && buffer_kbit>0 &&
        x264_mvc_predictive_limit( paired_bits, plan->future_headroom, n,
                                    (double)peak_kbps*1000./fps,
                                    (double)buffer_kbit*1000. ) ) goto done;
    double maximum=0;
    for( int order=0; order<n; ++order )
    {
        int i=by_out[order];
        double pair=target[0][i]+target[1][i];
        double scale=pair>0 ? paired_bits[order]/pair : 1.;
        target[0][i]*=scale;
        target[1][i]*=scale;
        maximum=fmax(maximum,paired_bits[order]);
    }
    /* x264 already uses its own pass-2 picture complexity allocation.
     * These bounded corrections steer that decision toward the paired RD
     * allocation without replacing native QP smoothing, mbtree, or VBV. */
    for(int v=0;v<2;v++)
    {
        double final_budget=0;
        for(int i=0;i<n;i++) final_budget+=target[v][i];
        int rate=(int)fmax(1.,floor(final_budget*fps/(1000.*n)+.5));
        if(rate>INT_MAX) goto done;
        plan->bitrate[v]=rate;
        /* Native pass-2 expects approximately the source's weighted picture
         * complexity; compare the joint optimizer allocation with that curve. */
        for(int i=0;i<n;i++)
        {
            const double expected=final_budget*weight[v][i]/sums[v];
            double factor=expected/fmax(128.,target[v][i]);
            /* A small correction rather than overriding x264's native RC. */
            factor=sqrt(factor);
            plan->qscale[v][i]=(float)fmin(1.75,fmax(.65,factor));
        }
    }
    plan->frames=n;
    plan->predicted_peak_bits=maximum;
    fprintf(stderr,"x264 MVC: per-frame joint RD plan %d pairs, target base=%d dependent=%d kb/s; "
                   "predicted max pair=%.0f bits; predictive shared VBV %d kb/s / %d kbit\n",
            n,plan->bitrate[0],plan->bitrate[1],maximum,peak_kbps,buffer_kbit);
    result=0;
done:
    costs_clean(&c[0]); costs_clean(&c[1]);
    free(target[0]);free(target[1]);free(weight[0]);free(weight[1]);free(by_out);
    free(paired_bits);
    if(result) x264_mvc_joint_plan_clean(plan);
    return result;
}
int x264_mvc_hrd_init( x264_mvc_hrd_t *h, int max_kbps, int buffer_kbit, double fps )
{
    memset(h,0,sizeof(*h));
    if(max_kbps<=0 || buffer_kbit<=0 || fps<=0 || !isfinite(fps)) return -1;
    h->capacity_bits=buffer_kbit*1000.;
    h->refill_bits=max_kbps*1000./fps;
    h->fill_bits=.90*h->capacity_bits;
    h->minimum_fill_bits=h->fill_bits;
    return 0;
}
int x264_mvc_hrd_commit( x264_mvc_hrd_t *h, size_t bytes )
{
    if(bytes>(size_t)(INT_MAX/8) * 1024u) return -1;
    h->fill_bits=fmin(h->capacity_bits,h->fill_bits+h->refill_bits)-8.*bytes;
    if(h->fill_bits<h->minimum_fill_bits) h->minimum_fill_bits=h->fill_bits;
    h->frames++;
    return h->fill_bits>=-0.5 ? 0 : -1;
}

