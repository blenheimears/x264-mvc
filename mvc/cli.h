/* x264 MVC CLI extension. GPL-2.0-or-later; x264 project contributors. */
#ifndef X264_MVC_CLI_H
#define X264_MVC_CLI_H
#include <stdint.h>
#include "x264.h"
#ifdef __cplusplus
extern "C" {
#endif
/* MVC options never change the default AVC encoding path. */
typedef struct {
    int enabled;
    int layout; /* 0=sbs, 1=top-bottom, 2=alternating frames, 3=separate */
    unsigned prediction_mask; /* I=1, P=2, B=4 */
    const char *right_filename;
    const char *dependent_filename;
    const char *output_filename;
    int user_fps;
    int requested_frames;
    /* 0 inherits the corresponding stock x264 rate-control parameter. */
    int dependent_bitrate;
    int dependent_vbv_maxrate;
    int dependent_vbv_bufsize;
    /* Combined two-view rate control. 0 disables it. */
    int combined_bitrate;
    int combined_vbv_maxrate;
    int combined_vbv_bufsize;
    float dependent_crf_offset;
    int view_vbv_maxrate_cap;
    int view_vbv_bufsize_cap;
    const char *rc_trace_filename;
} x264_mvc_cli_opts_t;
/* Returns 0 (picture read), 1 (EOF), -1 (read error). */
typedef int (*x264_mvc_read_picture)( void *opaque, x264_picture_t *picture, int64_t frame, int right );
int x264_mvc_cli_encode( const x264_param_t *param, const x264_mvc_cli_opts_t *opt,
                         x264_mvc_read_picture read_picture, void *opaque );
#ifdef __cplusplus
}
#endif
#endif
