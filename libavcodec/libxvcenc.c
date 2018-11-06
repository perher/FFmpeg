/*
 * libxvc encoder wrapper
 * Copyright (c) 2017 Divideon
 *
 * This file is part of FFmpeg.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <xvcenc.h>

#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avcodec.h"
#include "internal.h"

typedef struct libxvcEncoderContext {
    const AVClass *class;

    const xvc_encoder_api *api;
    xvc_encoder_parameters *params;
    xvc_encoder *encoder;
    xvc_enc_pic_buffer *rec_pic;
    int64_t dts;

    int qp;
    int tune_mode;
    int speed_mode;
    int max_keypic_distance;
    int closed_gop;
    int num_ref_pics;
    int internal_bitdepth;
    int sub_gop_length;
    char *explicit_encoder_settings;
} libxvcEncoderContext;

static av_cold int xvc_encode_close(AVCodecContext *avctx)
{
    libxvcEncoderContext *ctx = avctx->priv_data;

    if (ctx->params)
        ctx->api->parameters_destroy(ctx->params);
    if (ctx->encoder)
        ctx->api->encoder_destroy(ctx->encoder);
    if (ctx->rec_pic)
        ctx->api->picture_destroy(ctx->rec_pic);
    return 0;
}

static av_cold int xvc_encode_init(AVCodecContext *avctx)
{
    libxvcEncoderContext *ctx = avctx->priv_data;
    xvc_enc_return_code ret;

    ctx->api = xvc_encoder_api_get();
    ctx->params = ctx->api->parameters_create();
    if (!ctx->params) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate xvc enc param structure.\n");
        return AVERROR(ENOMEM);
    }

    ret = ctx->api->parameters_set_default(ctx->params);
    if (ret != XVC_ENC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot reset libxvc encoder parameters.\n");
        xvc_encode_close(avctx);
        return AVERROR(EINVAL);
    }

    ctx->params->threads = avctx->thread_count == 0 ? -1 : (avctx->thread_count == 1 ? 0 : avctx->thread_count);
    av_log(avctx, AV_LOG_INFO, "Using %d encoder threads\n", ctx->params->threads);

    ctx->params->framerate  = avctx->time_base.den / avctx->time_base.num *
        avctx->ticks_per_frame;
    ctx->params->width      = avctx->width;
    ctx->params->height     = avctx->height;

    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV420P12:
        ctx->params->chroma_format = XVC_ENC_CHROMA_FORMAT_420;
        break;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_YUV422P12:
        ctx->params->chroma_format = XVC_ENC_CHROMA_FORMAT_422;
        break;
    case AV_PIX_FMT_GBRP:
    case AV_PIX_FMT_GBRP10:
    case AV_PIX_FMT_GBRP12:
        ctx->params->chroma_format = XVC_ENC_CHROMA_FORMAT_444;
        break;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_YUV444P12:
        ctx->params->chroma_format = XVC_ENC_CHROMA_FORMAT_444;
        break;
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_GRAY10:
    case AV_PIX_FMT_GRAY12:
        ctx->params->chroma_format = XVC_ENC_CHROMA_FORMAT_MONOCHROME;
        break;
    }

    switch (avctx->pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_GBRP:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_GRAY8:
        ctx->params->input_bitdepth = 8;
        break;
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_GBRP10:
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_GRAY10:
        ctx->params->input_bitdepth = 10;
        break;
    case AV_PIX_FMT_YUV420P12:
    case AV_PIX_FMT_YUV422P12:
    case AV_PIX_FMT_GBRP12:
    case AV_PIX_FMT_YUV444P12:
    case AV_PIX_FMT_GRAY12:
        ctx->params->input_bitdepth = 12;
        break;
    }

    if (ctx->qp >= 0) {
        ctx->params->qp = ctx->qp;
    }
    if (ctx->tune_mode >= 0) {
        ctx->params->tune_mode = ctx->tune_mode;
    }
    if (ctx->speed_mode >= 0) {
        ctx->params->speed_mode = ctx->speed_mode;
    }
    if (ctx->max_keypic_distance >= 0) {
        ctx->params->max_keypic_distance = ctx->max_keypic_distance;
    }
    if (ctx->closed_gop >= 0) {
        ctx->params->closed_gop = ctx->closed_gop;
    }
    if (ctx->num_ref_pics >= 0) {
        ctx->params->num_ref_pics = ctx->num_ref_pics;
    }
    if (ctx->internal_bitdepth >= 0) {
        ctx->params->internal_bitdepth = ctx->internal_bitdepth;
    }
    if (ctx->sub_gop_length >= 0) {
        ctx->params->sub_gop_length = ctx->sub_gop_length;
    }
    if (ctx->explicit_encoder_settings) {
        ctx->params->explicit_encoder_settings = ctx->explicit_encoder_settings;
    }

    ret = ctx->api->parameters_check(ctx->params);
    if (ret != XVC_ENC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Invalid libxvc encoder parameters: %s\n",
            ctx->api->xvc_enc_get_error_text(ret));
        return AVERROR(EINVAL);
    }

    ctx->encoder = ctx->api->encoder_create(ctx->params);
    if (!ctx->encoder) {
        av_log(avctx, AV_LOG_ERROR, "Cannot open libxvc encoder.\n");
        xvc_encode_close(avctx);
        return AVERROR(EINVAL);
    }

    ctx->rec_pic = ctx->api->picture_create(ctx->encoder);
    if (!ctx->rec_pic) {
        av_log(avctx, AV_LOG_ERROR, "Cannot create output picture\n");
        xvc_encode_close(avctx);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int xvc_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *pic, int *got_packet)
{
    libxvcEncoderContext *ctx = avctx->priv_data;
    xvc_enc_return_code xvc_ret;
    xvc_enc_nal_unit *nal_units = NULL;
    int num_nal_units = 0;
    const uint8_t *plane_bytes[3];
    uint8_t *dst;
    int plane_stride[3];
    int payload = 0;
    int ret;
    int i;

    if (pic) {
        for (int i = 0; i < 3; i++) {
          plane_bytes[i] = pic->data[i];
          plane_stride[i] = pic->linesize[i];
        }
        xvc_ret = ctx->api->encoder_encode2(ctx->encoder, plane_bytes,
                                            plane_stride, &nal_units,
                                            &num_nal_units, ctx->rec_pic,
                                            pic->pts);
        if (xvc_ret != XVC_ENC_OK) {
            return AVERROR_EXTERNAL;
        }
    } else {
        xvc_ret = ctx->api->encoder_flush(ctx->encoder, &nal_units,
                                          &num_nal_units, ctx->rec_pic);
        if (xvc_ret != XVC_ENC_OK) {
            // return AVERROR_EXTERNAL;
        }
    }

    if (!num_nal_units) {
        return 0;
    }

    for (i = 0; i < num_nal_units; i++) {
        payload += nal_units[i].size;
    }

    ret = ff_alloc_packet(pkt, payload);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error getting output packet.\n");
        return ret;
    }

    dst = pkt->data;
    for (i = 0; i < num_nal_units; i++) {
        memcpy(dst, nal_units[i].bytes, nal_units[i].size);
        dst += nal_units[i].size;
        if (nal_units[i].stats.nal_unit_type == 16) {
            pkt->flags |= AV_PKT_FLAG_KEY;
        }
#if FF_API_CODED_FRAME
FF_DISABLE_DEPRECATION_WARNINGS
        if (nal_units[i].stats.nal_unit_type == 0 ||
            nal_units[i].stats.nal_unit_type == 1) {
            avctx->coded_frame->pict_type = AV_PICTURE_TYPE_I;
        } else if (nal_units[i].stats.nal_unit_type >= 2 ||
                   nal_units[i].stats.nal_unit_type <= 5){
            avctx->coded_frame->pict_type = AV_PICTURE_TYPE_P;
        }
FF_ENABLE_DEPRECATION_WARNINGS
#endif
    }

    pkt->pts = nal_units[0].user_data != 0 || num_nal_units == 1 ?
                        nal_units[0].user_data : nal_units[1].user_data;
    pkt->dts = ctx->dts - (ctx->sub_gop_length <= 0 ? 16 : ctx->sub_gop_length);
    ctx->dts++;

    *got_packet = 1;
    return 0;
}

static const enum AVPixelFormat xvc_csp_highbd[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUVA420P,
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_YUV440P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_YUV420P10,
    AV_PIX_FMT_YUV422P10,
    AV_PIX_FMT_YUV440P10,
    AV_PIX_FMT_YUV444P10,
    AV_PIX_FMT_YUV420P12,
    AV_PIX_FMT_YUV422P12,
    AV_PIX_FMT_YUV440P12,
    AV_PIX_FMT_YUV444P12,
    AV_PIX_FMT_GBRP,
    AV_PIX_FMT_GBRP10,
    AV_PIX_FMT_GBRP12,
    AV_PIX_FMT_NONE
};

static av_cold void xvc_encode_init_csp(AVCodec *codec)
{
    codec->pix_fmts = xvc_csp_highbd;
}

#define OFFSET(x) offsetof(libxvcEncoderContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
    { "qp",                     "set the xvc QP",                                        OFFSET(qp),        AV_OPT_TYPE_INT,  {.i64 = -1}, -1, 64, VE },
    { "tune",                   "tune",                                                  OFFSET(tune_mode), AV_OPT_TYPE_INT,  {.i64 = -1}, -1, INT_MAX, VE },
    { "speed-mode",             "speed mode (0=placebo, 1=slow (default), 2=fast",       OFFSET(speed_mode), AV_OPT_TYPE_INT,  {.i64 = -1}, -1, INT_MAX, VE },
    { "max-keypic-distance",    "max key-picture distance (keyint)",                     OFFSET(max_keypic_distance), AV_OPT_TYPE_INT,  {.i64 = -1}, -1, INT_MAX, VE },
    { "closed-gop",             "closed-gop",                                            OFFSET(closed_gop), AV_OPT_TYPE_INT,  {.i64 = -1}, -1, INT_MAX, VE },
    { "num-ref-pics",           "number of reference pictures",                          OFFSET(num_ref_pics),  AV_OPT_TYPE_INT,  {.i64 = -1}, -1, INT_MAX, VE },
    { "internal-bitdepth",      "internal bitdepth",                                     OFFSET(internal_bitdepth),  AV_OPT_TYPE_INT,  {.i64 = -1}, -1, 16, VE },
    { "sub-gop-length",         "sub-gop length",                                        OFFSET(sub_gop_length),  AV_OPT_TYPE_INT,  {.i64 = -1}, -1, 64, VE },
    { "explicit-encoder-settings", "explicit xvc encoder settings",                      OFFSET(explicit_encoder_settings),  AV_OPT_TYPE_STRING, { 0 }, 0, 0, VE },
    { NULL }
};

static const AVClass class = {
    .class_name = "libxvc",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault xvc_defaults[] = {
    { NULL },
};

AVCodec ff_libxvc_encoder = {
    .name             = "libxvc",
    .long_name        = NULL_IF_CONFIG_SMALL("libxvc xvc"),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_XVC,
    .priv_data_size   = sizeof(libxvcEncoderContext),
    .init             = xvc_encode_init,
    .init_static_data = xvc_encode_init_csp,
    .encode2          = xvc_encode_frame,
    .close            = xvc_encode_close,
    .priv_class       = &class,
    .defaults         = xvc_defaults,
    .capabilities     = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AUTO_THREADS,
};
