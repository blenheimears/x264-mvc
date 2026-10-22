/* Experimental cross-view rate-distortion planner / elementary-stream VBV model.
 * GPL-2.0-or-later. Independent implementation; no reference encoder code. */
#ifndef X264_MVC_JOINT_RC_H
#define X264_MVC_JOINT_RC_H
#include <stddef.h>
typedef struct
{
    float *qscale[2]; /* frame index in presentation order, immutable during pass 2 */
    int frames;
    int bitrate[2];
    double predicted_peak_bits;
    double *future_headroom; /* decode-order shared CPB reserve, in bits */
} x264_mvc_joint_plan_t;
/* First-pass stats only; no video pictures or bitstreams are retained. */
int x264_mvc_joint_plan( x264_mvc_joint_plan_t *, const char *, const char *,
                         int, int, double, int, int );
void x264_mvc_joint_plan_clean( x264_mvc_joint_plan_t * );
/* Deterministic horizon-limited advance budgeting; bits[] is modified in-place.
 * headroom[] records the forecast shared credit to preserve after each AU. */
int x264_mvc_predictive_limit( double *bits, double *headroom, int count,
                                double refill_bits, double capacity_bits );
typedef struct
{
    double fill_bits, capacity_bits, refill_bits;
    double minimum_fill_bits;
    size_t frames;
} x264_mvc_hrd_t;
int x264_mvc_hrd_init( x264_mvc_hrd_t *, int max_kbps, int buffer_kbit,
                       double fps );
/* Called exactly once per *completed* stereo access unit (decode order). */
int x264_mvc_hrd_commit( x264_mvc_hrd_t *, size_t bytes );
#endif
