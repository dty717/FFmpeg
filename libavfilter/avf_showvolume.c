/*
 * Copyright (c) 2015 Paul B Mahol
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/eval.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/xga_font_data.h"
#include "avfilter.h"
#include "formats.h"
#include "audio.h"
#include "video.h"
#include "internal.h"

static const char *const var_names[] = {   "VOLUME",   "CHANNEL",   "PEAK",        NULL };
enum                                   { VAR_VOLUME, VAR_CHANNEL, VAR_PEAK, VAR_VARS_NB };

typedef struct ShowVolumeContext {
    const AVClass *class;
    int w, h;
    int b;
    double f;
    AVRational frame_rate;
    char *color;
    int orientation;
    int step;
    float bgopacity;
    int mode;

    AVFrame *out;
    AVExpr *c_expr;
    int draw_text;
    int draw_volume;
    double *values;
    uint32_t *color_lut;
    float *max;
    float rms_factor;

    void (*meter)(float *src, int nb_samples, float *max, float factor);
} ShowVolumeContext;

#define OFFSET(x) offsetof(ShowVolumeContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption showvolume_options[] = {
    { "rate", "set video rate",  OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str="25"}, 0, INT_MAX, FLAGS },
    { "r",    "set video rate",  OFFSET(frame_rate), AV_OPT_TYPE_VIDEO_RATE, {.str="25"}, 0, INT_MAX, FLAGS },
    { "b", "set border width",   OFFSET(b), AV_OPT_TYPE_INT, {.i64=1}, 0, 5, FLAGS },
    { "w", "set channel width",  OFFSET(w), AV_OPT_TYPE_INT, {.i64=400}, 80, 8192, FLAGS },
    { "h", "set channel height", OFFSET(h), AV_OPT_TYPE_INT, {.i64=20}, 1, 900, FLAGS },
    { "f", "set fade",           OFFSET(f), AV_OPT_TYPE_DOUBLE, {.dbl=0.95}, 0, 1, FLAGS },
    { "c", "set volume color expression", OFFSET(color), AV_OPT_TYPE_STRING, {.str="PEAK*255+floor((1-PEAK)*255)*256+0xff000000"}, 0, 0, FLAGS },
    { "t", "display channel names", OFFSET(draw_text), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
    { "v", "display volume value", OFFSET(draw_volume), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
    { "o", "set orientation", OFFSET(orientation), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "orientation" },
    {   "h", "horizontal", 0, AV_OPT_TYPE_CONST, {.i64=0}, 0, 0, FLAGS, "orientation" },
    {   "v", "vertical",   0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, FLAGS, "orientation" },
    { "s", "set step size", OFFSET(step), AV_OPT_TYPE_INT, {.i64=0}, 0, 5, FLAGS },
    { "p", "set background opacity", OFFSET(bgopacity), AV_OPT_TYPE_FLOAT, {.dbl=0}, 0, 1, FLAGS },
    { "m", "set mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=0}, 0, 1, FLAGS, "mode" },
    {   "p", "peak", 0, AV_OPT_TYPE_CONST, {.i64=0}, 0, 0, FLAGS, "mode" },
    {   "r", "rms",  0, AV_OPT_TYPE_CONST, {.i64=1}, 0, 0, FLAGS, "mode" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(showvolume);

static av_cold int init(AVFilterContext *ctx)
{
    ShowVolumeContext *s = ctx->priv;
    int ret;

    if (s->color) {
        ret = av_expr_parse(&s->c_expr, s->color, var_names,
                            NULL, NULL, NULL, NULL, 0, ctx);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE };
    static const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_RGBA, AV_PIX_FMT_NONE };
    int ret;

    formats = ff_make_format_list(sample_fmts);
    if ((ret = ff_formats_ref(formats, &inlink->out_formats)) < 0)
        return ret;

    layouts = ff_all_channel_counts();
    if ((ret = ff_channel_layouts_ref(layouts, &inlink->out_channel_layouts)) < 0)
        return ret;

    formats = ff_all_samplerates();
    if ((ret = ff_formats_ref(formats, &inlink->out_samplerates)) < 0)
        return ret;

    formats = ff_make_format_list(pix_fmts);
    if ((ret = ff_formats_ref(formats, &outlink->in_formats)) < 0)
        return ret;

    return 0;
}

static void find_peak(float *src, int nb_samples, float *peak, float factor)
{
    int i;

    *peak = 0;
    for (i = 0; i < nb_samples; i++)
        *peak = FFMAX(*peak, FFABS(src[i]));
}

static void find_rms(float *src, int nb_samples, float *rms, float factor)
{
    int i;

    for (i = 0; i < nb_samples; i++)
        *rms += factor * (src[i] * src[i] - *rms);
}

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ShowVolumeContext *s = ctx->priv;
    int nb_samples;

    nb_samples = FFMAX(1024, ((double)inlink->sample_rate / av_q2d(s->frame_rate)) + 0.5);
    inlink->partial_buf_size =
    inlink->min_samples =
    inlink->max_samples = nb_samples;
    s->values = av_calloc(inlink->channels * VAR_VARS_NB, sizeof(double));
    if (!s->values)
        return AVERROR(ENOMEM);

    s->color_lut = av_calloc(s->w, sizeof(*s->color_lut) * inlink->channels);
    if (!s->color_lut)
        return AVERROR(ENOMEM);

    s->max = av_calloc(inlink->channels, sizeof(*s->max));
    if (!s->max)
        return AVERROR(ENOMEM);

    s->rms_factor = 10000. / inlink->sample_rate;

    switch (s->mode) {
    case 0: s->meter = find_peak; break;
    case 1: s->meter = find_rms;  break;
    default: return AVERROR_BUG;
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    ShowVolumeContext *s = outlink->src->priv;
    AVFilterLink *inlink = outlink->src->inputs[0];
    int ch;

    if (s->orientation) {
        outlink->h = s->w;
        outlink->w = s->h * inlink->channels + (inlink->channels - 1) * s->b;
    } else {
        outlink->w = s->w;
        outlink->h = s->h * inlink->channels + (inlink->channels - 1) * s->b;
    }

    outlink->sample_aspect_ratio = (AVRational){1,1};
    outlink->frame_rate = s->frame_rate;

    for (ch = 0; ch < inlink->channels; ch++) {
        int i;

        for (i = 0; i < s->w; i++) {
            float max = i / (float)(s->w - 1);

            s->values[ch * VAR_VARS_NB + VAR_PEAK] = max;
            s->values[ch * VAR_VARS_NB + VAR_VOLUME] = 20.0 * log10(max);
            s->values[ch * VAR_VARS_NB + VAR_CHANNEL] = ch;
            s->color_lut[ch * s->w + i] = av_expr_eval(s->c_expr, &s->values[ch * VAR_VARS_NB], NULL);
        }
    }

    return 0;
}

static void drawtext(AVFrame *pic, int x, int y, const char *txt, int o)
{
    const uint8_t *font;
    int font_height;
    int i;

    font = avpriv_cga_font,   font_height =  8;

    for (i = 0; txt[i]; i++) {
        int char_y, mask;

        if (o) { /* vertical orientation */
            for (char_y = font_height - 1; char_y >= 0; char_y--) {
                uint8_t *p = pic->data[0] + (y + i * 10) * pic->linesize[0] + x * 4;
                for (mask = 0x80; mask; mask >>= 1) {
                    if (font[txt[i] * font_height + font_height - 1 - char_y] & mask)
                        AV_WN32(&p[char_y * 4], ~AV_RN32(&p[char_y * 4]));
                    p += pic->linesize[0];
                }
            }
        } else { /* horizontal orientation */
            uint8_t *p = pic->data[0] + y * pic->linesize[0] + (x + i * 8) * 4;
            for (char_y = 0; char_y < font_height; char_y++) {
                for (mask = 0x80; mask; mask >>= 1) {
                    if (font[txt[i] * font_height + char_y] & mask)
                        AV_WN32(p, ~AV_RN32(p));
                    p += 4;
                }
                p += pic->linesize[0] - 8 * 4;
            }
        }
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *insamples)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    ShowVolumeContext *s = ctx->priv;
    const int step = s->step;
    int c, i, j, k;
    AVFrame *out;

    if (!s->out || s->out->width  != outlink->w ||
                   s->out->height != outlink->h) {
        av_frame_free(&s->out);
        s->out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!s->out) {
            av_frame_free(&insamples);
            return AVERROR(ENOMEM);
        }

        for (i = 0; i < outlink->h; i++) {
            uint32_t *dst = (uint32_t *)(s->out->data[0] + i * s->out->linesize[0]);
            const uint32_t bg = (uint32_t)(s->bgopacity * 255) << 24;

            for (j = 0; j < outlink->w; j++)
                AV_WN32A(dst + j, bg);
        }
    }
    s->out->pts = insamples->pts;

    if (s->f < 1.) {
        for (j = 0; j < outlink->h; j++) {
            uint8_t *dst = s->out->data[0] + j * s->out->linesize[0];
            const uint32_t alpha = s->bgopacity * 255;

            for (k = 0; k < outlink->w; k++) {
                dst[k * 4 + 0] = FFMAX(dst[k * 4 + 0] * s->f, 0);
                dst[k * 4 + 1] = FFMAX(dst[k * 4 + 1] * s->f, 0);
                dst[k * 4 + 2] = FFMAX(dst[k * 4 + 2] * s->f, 0);
                dst[k * 4 + 3] = FFMAX(dst[k * 4 + 3] * s->f, alpha);
            }
        }
    }

    if (s->orientation) { /* vertical */
        for (c = 0; c < inlink->channels; c++) {
            float *src = (float *)insamples->extended_data[c];
            uint32_t *lut = s->color_lut + s->w * c;
            float max;

            s->meter(src, insamples->nb_samples, &s->max[c], s->rms_factor);
            max = s->max[c];

            s->values[c * VAR_VARS_NB + VAR_VOLUME] = 20.0 * log10(max);
            max = av_clipf(max, 0, 1);

            for (j = outlink->h - outlink->h * max; j < s->w; j++) {
                uint8_t *dst = s->out->data[0] + j * s->out->linesize[0] + c * (s->b + s->h) * 4;
                for (k = 0; k < s->h; k++) {
                    AV_WN32A(&dst[k * 4], lut[s->w - j - 1]);
                    if (j & step)
                        j += step;
                }
            }

            if (s->h >= 8 && s->draw_text) {
                const char *channel_name = av_get_channel_name(av_channel_layout_extract_channel(insamples->channel_layout, c));
                if (!channel_name)
                    continue;
                drawtext(s->out, c * (s->h + s->b) + (s->h - 10) / 2, outlink->h - 35, channel_name, 1);
            }
        }
    } else { /* horizontal */
        for (c = 0; c < inlink->channels; c++) {
            float *src = (float *)insamples->extended_data[c];
            uint32_t *lut = s->color_lut + s->w * c;
            float max;

            s->meter(src, insamples->nb_samples, &s->max[c], s->rms_factor);
            max = s->max[c];

            s->values[c * VAR_VARS_NB + VAR_VOLUME] = 20.0 * log10(max);
            max = av_clipf(max, 0, 1);

            for (j = 0; j < s->h; j++) {
                uint8_t *dst = s->out->data[0] + (c * s->h + c * s->b + j) * s->out->linesize[0];

                for (k = 0; k < s->w * max; k++) {
                    AV_WN32A(dst + k * 4, lut[k]);
                    if (k & step)
                        k += step;
                }
            }

            if (s->h >= 8 && s->draw_text) {
                const char *channel_name = av_get_channel_name(av_channel_layout_extract_channel(insamples->channel_layout, c));
                if (!channel_name)
                    continue;
                drawtext(s->out, 2, c * (s->h + s->b) + (s->h - 8) / 2, channel_name, 0);
            }
        }
    }

    av_frame_free(&insamples);
    out = av_frame_clone(s->out);
    if (!out)
        return AVERROR(ENOMEM);
    av_frame_make_writable(out);

    for (c = 0; c < inlink->channels && s->draw_volume; c++) {
        char buf[16];
        if (s->orientation) {
            if (s->h >= 8) {
                snprintf(buf, sizeof(buf), "%.2f", s->values[c * VAR_VARS_NB + VAR_VOLUME]);
                drawtext(out, c * (s->h + s->b) + (s->h - 8) / 2, 2, buf, 1);
            }
        } else {
            if (s->h >= 8) {
                snprintf(buf, sizeof(buf), "%.2f", s->values[c * VAR_VARS_NB + VAR_VOLUME]);
                drawtext(out, FFMAX(0, s->w - 8 * (int)strlen(buf)), c * (s->h + s->b) + (s->h - 8) / 2, buf, 0);
            }
        }
    }

    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ShowVolumeContext *s = ctx->priv;

    av_frame_free(&s->out);
    av_expr_free(s->c_expr);
    av_freep(&s->values);
    av_freep(&s->color_lut);
    av_freep(&s->max);
}

static const AVFilterPad showvolume_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .config_props = config_input,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad showvolume_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_avf_showvolume = {
    .name          = "showvolume",
    .description   = NULL_IF_CONFIG_SMALL("Convert input audio volume to video output."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(ShowVolumeContext),
    .inputs        = showvolume_inputs,
    .outputs       = showvolume_outputs,
    .priv_class    = &showvolume_class,
};
