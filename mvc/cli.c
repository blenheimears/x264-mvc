/* Two-view MVC encoding loop for the stock x264 CLI.
 * GPL-2.0-or-later; x264 project contributors. No JMVC code is included. */
#include "cli.h"
#include "nal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

typedef struct { x264_t *encoder; x264_param_t param; int level; char stats[1100]; } mvc_view_t;

static double mvc_clip( double v, double lo, double hi )
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int mvc_open( mvc_view_t *view, const x264_param_t *source, unsigned mask,
                     int dependent, const x264_mvc_cli_opts_t *opt, int width, int height,
                     const char *stats, double pass2_share )
{
    view->param = *source;
    x264_param_t *p = &view->param;
    /* Combined rate control starts from an even split; mvc_rc_update() then
     * redistributes the shared budget according to what each view consumes. */
    double split = pass2_share >= 0 ? (dependent ? 1.0 - pass2_share : pass2_share) : 0.5;
    if( opt->combined_bitrate )
    {
        if( pass2_share >= 0 )
        {
            p->rc.i_rc_method = X264_RC_ABR;
            p->rc.i_bitrate = (int)(opt->combined_bitrate * split + 0.5);
            if( p->rc.i_bitrate < 1 ) p->rc.i_bitrate = 1;
        }
        else
        {
            p->rc.i_rc_method = X264_RC_CRF;
            p->rc.i_bitrate = 0;
            if( dependent )
                p->rc.f_rf_constant = (float)mvc_clip( p->rc.f_rf_constant + opt->dependent_crf_offset, 0, 51 );
        }
    }
    else if( dependent && opt->combined_vbv_maxrate && p->rc.i_rc_method == X264_RC_CRF )
        p->rc.f_rf_constant = (float)mvc_clip( p->rc.f_rf_constant + opt->dependent_crf_offset, 0, 51 );
    if( opt->combined_vbv_maxrate )
        p->rc.i_vbv_max_bitrate = (int)(opt->combined_vbv_maxrate * split + 0.5);
    if( opt->combined_vbv_bufsize )
        p->rc.i_vbv_buffer_size = (int)(opt->combined_vbv_bufsize * split + 0.5);
    if( opt->view_vbv_maxrate_cap && p->rc.i_vbv_max_bitrate > opt->view_vbv_maxrate_cap )
        p->rc.i_vbv_max_bitrate = opt->view_vbv_maxrate_cap;
    if( opt->view_vbv_bufsize_cap && p->rc.i_vbv_buffer_size > opt->view_vbv_bufsize_cap )
        p->rc.i_vbv_buffer_size = opt->view_vbv_bufsize_cap;
    if( stats )
    {
        snprintf( view->stats, sizeof(view->stats), "%s.%s", stats, dependent ? "dep" : "base" );
        p->rc.psz_stat_out = p->rc.psz_stat_in = view->stats;
    }
    if( dependent )
    {
        if( opt->dependent_bitrate )
        {
            p->rc.i_rc_method = X264_RC_ABR;
            p->rc.i_bitrate = opt->dependent_bitrate;
        }
        if( opt->dependent_vbv_maxrate )
            p->rc.i_vbv_max_bitrate = opt->dependent_vbv_maxrate;
        if( opt->dependent_vbv_bufsize )
            p->rc.i_vbv_buffer_size = opt->dependent_vbv_bufsize;
    }
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
    p->b_repeat_headers = 1;
    p->b_open_gop = 0;
    p->i_bframe_adaptive = X264_B_ADAPT_NONE;
    p->i_scenecut_threshold = 0;
    if( mask ) p->b_sliced_threads = 1;
    if( dependent && mask ) p->analyse.i_weighted_pred = X264_WEIGHTP_NONE;
    if( p->b_interlaced || p->b_fake_interlaced ||
        p->i_frame_reference > 8 || p->i_dpb_size > 8 ||
        x264_param_apply_profile( p, "high" ) )
        return -1;
    view->encoder = x264_encoder_open( p );
    return view->encoder ? 0 : -1;
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
    view->level = level;
    (void)out; (void)dependent; (void)mask;
    return 0;
}
/* Shared two-view rate control, evaluated after every access unit.
 * The CRF is common to both views (equal quality), so a view that needs more
 * bits to reach it takes more of the combined bitrate. A combined VBV bucket is
 * fed with the real sizes of both views; when it runs low the CRF is raised at
 * once. The combined VBV maxrate/buffer are divided between the views by their
 * smoothed demand, so the sum of the per-view buckets never exceeds the
 * combined one. */
typedef struct
{
    int use_bitrate, use_vbv;
    double cap_maxrate, cap_bufsize;
    double target_kbps, maxrate, bufsize;
    double base_crf, crf, boost;
    double crf_offset;
    double share_base;
    double fps;
    double smooth_bits;
    double smooth_share;
    double total_bits, view_bits[2];
    double fill, min_fill;
    int64_t total_pairs, underflows, reconfigs;
    double applied_crf, applied_share;
    FILE *trace;
} mvc_rc_t;

static void mvc_rc_init( mvc_rc_t *rc, const x264_param_t *settings, const x264_mvc_cli_opts_t *opt )
{
    memset( rc, 0, sizeof(*rc) );
    rc->use_bitrate = opt->combined_bitrate > 0;
    rc->use_vbv = opt->combined_vbv_maxrate > 0 && opt->combined_vbv_bufsize > 0;
    rc->target_kbps = opt->combined_bitrate;
    rc->cap_maxrate = opt->view_vbv_maxrate_cap;
    rc->cap_bufsize = opt->view_vbv_bufsize_cap;
    rc->maxrate = opt->combined_vbv_maxrate;
    rc->bufsize = opt->combined_vbv_bufsize;
    rc->base_crf = rc->crf = rc->applied_crf = settings->rc.f_rf_constant;
    rc->crf_offset = opt->dependent_crf_offset;
    rc->share_base = rc->applied_share = rc->smooth_share = 0.5;
    rc->fps = (double)settings->i_fps_num / settings->i_fps_den;
    rc->fill = rc->min_fill = rc->bufsize * 1000.0 * 0.9;
}

static int mvc_rc_apply( mvc_rc_t *rc, mvc_view_t *view, int dependent )
{
    x264_param_t *p = &view->param;
    double share = dependent ? 1.0 - rc->share_base : rc->share_base;
    if( rc->use_bitrate || rc->use_vbv )
        p->rc.f_rf_constant = (float)mvc_clip( rc->crf + (dependent ? rc->crf_offset : 0), 0, 51 );
    if( rc->use_vbv && !p->i_nal_hrd )
    {
        double mr = rc->maxrate * share, bs = rc->bufsize * share;
        if( rc->cap_maxrate > 0 && mr > rc->cap_maxrate ) mr = rc->cap_maxrate;
        if( rc->cap_bufsize > 0 && bs > rc->cap_bufsize ) bs = rc->cap_bufsize;
        p->rc.i_vbv_max_bitrate = (int)(mr + 0.5);
        p->rc.i_vbv_buffer_size = (int)(bs + 0.5);
    }
    return x264_encoder_reconfig( view->encoder, p );
}

static int mvc_rc_update( mvc_rc_t *rc, mvc_view_t *base, mvc_view_t *dep, int base_bytes, int dep_bytes )
{
    double bits[2] = { 8.0 * base_bytes, 8.0 * dep_bytes };
    double au_bits = bits[0] + bits[1];
    double window = rc->fps < 1 ? 1 : rc->fps;
    double frac = 1;
    rc->total_pairs++;
    rc->total_bits += au_bits;
    rc->view_bits[0] += bits[0];
    rc->view_bits[1] += bits[1];
    if( rc->total_pairs == 1 )
        rc->smooth_bits = au_bits;
    rc->smooth_bits += (au_bits - rc->smooth_bits) / window;
    if( au_bits > 0 )
        rc->smooth_share += (bits[0] / au_bits - rc->smooth_share) / window;

    if( rc->use_vbv )
    {
        rc->fill += rc->maxrate * 1000.0 / rc->fps - au_bits;
        if( rc->fill < 0 ) { rc->underflows++; rc->fill = 0; }
        if( rc->fill > rc->bufsize * 1000.0 ) rc->fill = rc->bufsize * 1000.0;
        if( rc->fill < rc->min_fill ) rc->min_fill = rc->fill;
        frac = rc->fill / (rc->bufsize * 1000.0);
    }
    if( rc->use_bitrate )
    {
        double target = rc->target_kbps * 1000.0 / rc->fps;
        double shortr = rc->smooth_bits / target;
        double longr = rc->total_bits / (rc->total_pairs * target);
        double ratio = 0.5 * (shortr + longr);
        if( ratio > 0 )
            rc->crf += mvc_clip( 0.02 * 6.0 * log( ratio ) / log( 2.0 ), -0.2, 0.2 );
        rc->crf = mvc_clip( rc->crf, 5.0, 51.0 );
    }
    if( rc->use_vbv )
    {
        double attack = frac < 0.6 ? (0.6 - frac) * 12.0 : 0;
        rc->boost = rc->boost * 0.98;
        if( attack > rc->boost ) rc->boost = attack;
        if( !rc->use_bitrate )
            rc->crf = rc->base_crf;
    }
    rc->share_base = mvc_clip( rc->smooth_share, 0.05, 0.95 );
    double eff_crf = mvc_clip( rc->crf + rc->boost, 5.0, 51.0 );
    if( rc->trace )
        fprintf( rc->trace, "%" PRId64 ",%d,%d,%.3f,%.4f,%.1f\n", rc->total_pairs - 1, base_bytes, dep_bytes,
                 eff_crf, rc->share_base, rc->use_vbv ? rc->fill / 1000.0 : 0.0 );
    if( !rc->use_bitrate && !rc->use_vbv )
        return 0;
    if( fabs( eff_crf - rc->applied_crf ) < 0.02 && fabs( rc->share_base - rc->applied_share ) < 0.02 )
        return 0;
    double stored = rc->crf;
    rc->crf = eff_crf;
    int err = mvc_rc_apply( rc, base, 0 ) || mvc_rc_apply( rc, dep, 1 );
    rc->crf = stored;
    rc->applied_crf = eff_crf;
    rc->applied_share = rc->share_base;
    rc->reconfigs++;
    return err ? -1 : 0;
}

static void mvc_rc_report( const mvc_rc_t *rc )
{
    if( !rc->total_pairs || rc->fps <= 0 ) return;
    double secs = rc->total_pairs / rc->fps;
    fprintf( stderr, "x264 MVC: %.2f kb/s total (base %.2f, dependent %.2f)",
             rc->total_bits / secs / 1000.0, rc->view_bits[0] / secs / 1000.0, rc->view_bits[1] / secs / 1000.0 );
    if( rc->use_vbv )
        fprintf( stderr, ", combined VBV minimum fill %.1f%%, %" PRId64 " underflows",
                 100.0 * rc->min_fill / (rc->bufsize * 1000.0), rc->underflows );
    fprintf( stderr, "\n" );
}
static int mvc_encode_pair( mvc_view_t *base, mvc_view_t *dep, FILE *out, FILE *depout,
                            unsigned mask, x264_picture_t *left, x264_picture_t *right,
                            int64_t *written, mvc_rc_t *rc )
{
    x264_nal_t *base_nal = NULL, *dep_nal = NULL;
    int base_count = 0, dep_count = 0;
    x264_picture_t out_base = {0}, out_dep = {0};
    int base_size = x264_encoder_encode( base->encoder, &base_nal, &base_count, left, &out_base );
    if( base_size < 0 ) { fprintf( stderr, "x264 [error]: base view encode failed\n" ); return -1; }
    if( mask && base_size && x264_encoder_mvc_reference( dep->encoder, &out_base ) )
    {
        fprintf( stderr, "x264 [error]: cannot hand the base-view reconstruction to the dependent view\n" );
        return -1;
    }
    int dep_size = x264_encoder_encode( dep->encoder, &dep_nal, &dep_count, right, &out_dep );
    if( dep_size < 0 ) { fprintf( stderr, "x264 [error]: dependent view encode failed\n" ); return -1; }
    if( !!base_size != !!dep_size )
    {
        fprintf( stderr, "x264 [error]: the views produced output at different times\n" );
        return -1;
    }
    if( base_size )
    {
        if( out_base.i_pts != out_dep.i_pts || out_base.i_dts != out_dep.i_dts ||
            out_base.i_type != out_dep.i_type || out_base.b_keyframe != out_dep.b_keyframe )
        {
            fprintf( stderr, "x264 [error]: the views disagree on frame type or timing (pair %" PRId64 ")\n", *written );
            return -1;
        }
        if( x264_mvc_write_nals( out, base_nal, base_count, 0, base->level, mask ) ||
            x264_mvc_write_nals( depout, dep_nal, dep_count, 1, base->level, mask ) )
            return -1;
        ++*written;
        if( mvc_rc_update( rc, base, dep, base_size, dep_size ) ) return -1;
    }
    return 0;
}
int x264_mvc_cli_encode( const x264_param_t *param, const x264_mvc_cli_opts_t *opt,
                         x264_mvc_read_picture reader, void *opaque )
{
    int result = -1;
    const char *why = "verify the input layouts and constraints";
    int allocated = 0;
    int srcw = 0, srch = 0, w = 0, h = 0;
    int64_t submitted = 0, written = 0;
    FILE *output = NULL, *dependent_output = NULL;
    mvc_view_t base = {0}, dependent = {0};
    x264_picture_t src = {0}, src_right = {0}, left = {0}, right = {0};
    x264_param_t settings;
    mvc_rc_t rc;
    x264_mvc_cli_opts_t ropt;
    const char *stats = NULL;
    double pass2_share = -1;
    FILE *trace = NULL;
    if( !param || !opt || !reader || !opt->enabled ) goto done;
    ropt = *opt;
    opt = &ropt;
    if( (ropt.combined_bitrate || ropt.combined_vbv_maxrate || ropt.combined_vbv_bufsize) &&
        (ropt.dependent_bitrate || ropt.dependent_vbv_maxrate || ropt.dependent_vbv_bufsize) )
    {
        why = "--mvc-combined-* and --mvc-dependent-* rate-control options cannot be mixed";
        goto done;
    }
    /* Stock rate-control options describe the stream as a whole, unless the
     * dependent view is configured explicitly (then they apply to the base view). */
    if( !ropt.dependent_bitrate && !ropt.dependent_vbv_maxrate && !ropt.dependent_vbv_bufsize )
    {
        if( !ropt.combined_bitrate && param->rc.i_rc_method == X264_RC_ABR && param->rc.i_bitrate > 0 )
            ropt.combined_bitrate = param->rc.i_bitrate;
        if( !ropt.combined_vbv_maxrate ) ropt.combined_vbv_maxrate = param->rc.i_vbv_max_bitrate;
        if( !ropt.combined_vbv_bufsize ) ropt.combined_vbv_bufsize = param->rc.i_vbv_buffer_size;
    }
    if( param->rc.b_stat_write || param->rc.b_stat_read )
    {
        stats = param->rc.b_stat_read ? param->rc.psz_stat_in : param->rc.psz_stat_out;
        if( !stats || strlen( stats ) > 1000 ) goto done;
        if( param->rc.b_stat_read )
        {
            char side[1100];
            double bits[2] = {0, 0};
            FILE *f;
            snprintf( side, sizeof(side), "%s.mvc", stats );
            f = fopen( side, "r" );
            if( !f || fscanf( f, "%lf %lf", &bits[0], &bits[1] ) != 2 || bits[0] + bits[1] <= 0 )
            {
                if( f ) fclose( f );
                why = "second pass needs the first-pass statistics of both views";
                goto done;
            }
            fclose( f );
            if( !ropt.combined_bitrate )
            {
                why = "second pass needs a bitrate (--bitrate or --mvc-combined-bitrate)";
                goto done;
            }
            pass2_share = mvc_clip( bits[0] / (bits[0] + bits[1]), 0.05, 0.95 );
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
    if( opt->requested_frames ) settings.i_frame_total = opt->requested_frames;
    else if( opt->layout == 2 && settings.i_frame_total > 0 ) settings.i_frame_total /= 2;
    if( !opt->output_filename ) goto done;
    output = !strcmp( opt->output_filename, "-" ) ? stdout : fopen( opt->output_filename, "wb" );
    if( !output ) goto done;
    dependent_output = output;
    if( opt->dependent_filename )
    {
        dependent_output = !strcmp( opt->dependent_filename, "-" ) ? stdout :
            fopen( opt->dependent_filename, "wb" );
        if( !dependent_output || dependent_output == output ) goto done;
    }
    if( mvc_open( &base, &settings, opt->prediction_mask, 0, opt, w, h, stats, pass2_share ) ||
        mvc_open( &dependent, &settings, opt->prediction_mask, 1, opt, w, h, stats, pass2_share ) )
    {
        why = "an encoder could not be opened; check the settings (interlaced, reference count and profile limits apply)";
        goto done;
    }
    if( pass2_share >= 0 )
        ropt.combined_bitrate = ropt.combined_vbv_maxrate = ropt.combined_vbv_bufsize = 0;
    mvc_rc_init( &rc, &settings, opt );
    if( opt->rc_trace_filename )
    {
        trace = fopen( opt->rc_trace_filename, "w" );
        if( !trace ) { why = "cannot open the rate-control trace file"; goto done; }
        fprintf( trace, "pair,base_bytes,dependent_bytes,crf,base_share,vbv_fill_kbit\n" );
        rc.trace = trace;
    }
    if( x264_picture_alloc( &src, X264_CSP_I420, srcw, srch ) ) goto done;
    allocated = 1;
    if( x264_picture_alloc( &src_right, X264_CSP_I420, srcw, srch ) ) goto done;
    allocated = 2;
    if( x264_picture_alloc( &left, X264_CSP_I420, w, h ) ) goto done;
    allocated = 3;
    if( x264_picture_alloc( &right, X264_CSP_I420, w, h ) ) goto done;
    allocated = 4;
    if( mvc_headers( &base, output, 0, opt->prediction_mask ) ||
        mvc_headers( &dependent, dependent_output, 1, opt->prediction_mask ) ) goto done;
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
        if( mvc_encode_pair( &base, &dependent, output, dependent_output,
                             opt->prediction_mask, &left, &right, &written, &rc ) ) goto done;
        submitted++;
    }
    while( x264_encoder_delayed_frames( base.encoder ) )
        if( mvc_encode_pair( &base, &dependent, output, dependent_output,
                             opt->prediction_mask, NULL, NULL, &written, &rc ) ) goto done;
    if( x264_encoder_delayed_frames( dependent.encoder ) || written != submitted ||
        fflush( output ) || (dependent_output != output && fflush( dependent_output )) ) goto done;
    fprintf( stderr, "x264 MVC: %" PRId64 " stereo pairs encoded\n", written );
    mvc_rc_report( &rc );
    if( param->rc.b_stat_write && stats )
    {
        char side[1100];
        FILE *f;
        snprintf( side, sizeof(side), "%s.mvc", stats );
        f = fopen( side, "w" );
        if( !f || fprintf( f, "%.0f %.0f\n", rc.view_bits[0], rc.view_bits[1] ) < 0 || fclose( f ) )
        {
            why = "cannot write the first-pass statistics";
            goto done;
        }
    }
    result = 0;
done:
    if( trace ) fclose( trace );
    if( result ) fprintf( stderr, "x264 [error]: MVC encode failed: %s\n", why );
    if( allocated >= 4 ) x264_picture_clean( &right );
    if( allocated >= 3 ) x264_picture_clean( &left );
    if( allocated >= 2 ) x264_picture_clean( &src_right );
    if( allocated >= 1 ) x264_picture_clean( &src );
    if( dependent.encoder ) x264_encoder_close( dependent.encoder );
    if( base.encoder ) x264_encoder_close( base.encoder );
    if( dependent_output && dependent_output != stdout && dependent_output != output ) fclose( dependent_output );
    if( output && output != stdout ) fclose( output );
    return result;
}
