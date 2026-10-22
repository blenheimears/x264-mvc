/* Copyright (C) 2026 the x264-MVC contributors.
 * GPL-2.0-or-later. Decode-order pair accounting and advisory VBV reservations.
 * This is NOT normative Annex-H/MVC operation-point HRD signalling. */
#ifndef X264_MVC_SHARED_VBV_H
#define X264_MVC_SHARED_VBV_H
#include <stdint.h>
#include "../common/osdep.h"

typedef struct x264_mvc_shared_vbv_t x264_mvc_shared_vbv_t;
/* Snapshot of the first terminal shared-VBV error.  A debit greater than
 * min(capacity, before+refill) is a genuine modelled elementary-stream CPB
 * violation; invalid-order denotes a scheduling/accounting bug instead. */
typedef struct
{
    int64_t order;
    const char *reason;
    double before, refill, actual[2], capacity;
} x264_mvc_vbv_failure_t;

x264_mvc_shared_vbv_t *x264_mvc_shared_vbv_create( int max_kbps, int buffer_kbit, double fps );
void x264_mvc_shared_vbv_destroy( x264_mvc_shared_vbv_t * );
/* Copy a decode-order forecast from MVC first-pass statistics.  Optional:
 * single-pass and --mvc-no-joint-ratecontrol retain the original behavior. */
int x264_mvc_shared_vbv_set_forecast( x264_mvc_shared_vbv_t *,
                                      const double *headroom_bits, int count );
/* A reservation is indexed by decode-order access unit (not input PTS).
 * It is always made before encoding and settled after ratecontrol_end.
 * Estimated bits are supplied by x264's existing rate estimator.
 * Only completed *pairs*, committed in decode order, can signal a real
 * aggregate CPB violation; reservations do not impose hard size ceilings. */
int x264_mvc_shared_vbv_reserve( x264_mvc_shared_vbv_t *, int64_t decode_order,
                                 int eye, double estimated_bits, double *allowed_bits );
int x264_mvc_shared_vbv_settle( x264_mvc_shared_vbv_t *, int64_t decode_order,
                                int eye, double actual_bits );
int x264_mvc_shared_vbv_status( x264_mvc_shared_vbv_t *, double *min_fill,
                                double *available, int *inflight );
int x264_mvc_shared_vbv_failure( x264_mvc_shared_vbv_t *,
                                 x264_mvc_vbv_failure_t * );
#endif
