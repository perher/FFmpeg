/*
 * libxvc decoder wrapper
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

#include <xvcdec.h>

#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "internal.h"

typedef struct libxvcDecoderContext {
    const AVClass *class;

    const xvc_decoder_api *api;
    xvc_decoder_parameters *params;
    xvc_decoder *decoder;
    xvc_decoded_picture *dec_pic;
} libxvcDecoderContext;

static av_cold int xvc_dec_close(AVCodecContext *avctx)
{
    libxvcDecoderContext *ctx = avctx->priv_data;
    if (ctx->params)
        ctx->api->parameters_destroy(ctx->params);
    if (ctx->decoder)
        ctx->api->decoder_destroy(ctx->decoder);
    if (ctx->dec_pic)
        ctx->api->picture_destroy(ctx->dec_pic);
    return 0;
}

static av_cold int xvc_init(AVCodecContext *avctx)
{
    libxvcDecoderContext *ctx = avctx->priv_data;
    xvc_dec_return_code ret;

    ctx->api = xvc_decoder_api_get();
    ctx->params = ctx->api->parameters_create();
    if (!ctx->params) {
        av_log(avctx, AV_LOG_ERROR, "Could not allocate xvc decoder param structure.\n");
        return AVERROR(ENOMEM);
    }

    ret = ctx->api->parameters_set_default(ctx->params);
    if (ret != XVC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Cannot reset xvc decoder parameters.\n");
        xvc_dec_close(avctx);
        return AVERROR(EINVAL);
    }

    ctx->params->threads = avctx->thread_count == 0 ? -1 : (avctx->thread_count == 1 ? 0 : avctx->thread_count);
    av_log(avctx, AV_LOG_INFO, "Using %d decoder threads\n", ctx->params->threads);

    ret = ctx->api->parameters_check(ctx->params);
    if (ret != XVC_DEC_OK) {
        av_log(avctx, AV_LOG_ERROR, "Invalid xvc decoder parameters: %s\n",
            ctx->api->xvc_dec_get_error_text(ret));
        xvc_dec_close(avctx);
        return AVERROR(EINVAL);
    }

    ctx->decoder = ctx->api->decoder_create(ctx->params);
    if (!ctx->decoder) {
        av_log(avctx, AV_LOG_ERROR, "Cannot open xvc decoder.\n");
        xvc_dec_close(avctx);
        return AVERROR(EINVAL);
    }

    ctx->dec_pic = ctx->api->picture_create(ctx->decoder);
    if (!ctx->dec_pic) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create output picture\n");
        xvc_dec_close(avctx);
        return AVERROR(EINVAL);
    }
    return 0;
}

// returns 0 on success, AVERROR_INVALIDDATA otherwise
static int set_pix_fmt(AVCodecContext *avctx, xvc_dec_pic_stats *img)
{
    //static const enum AVColorSpace colorspaces[8] = {
    //    AVCOL_SPC_UNSPECIFIED, AVCOL_SPC_BT470BG, AVCOL_SPC_BT709, AVCOL_SPC_SMPTE170M,
    //    AVCOL_SPC_SMPTE240M, AVCOL_SPC_BT2020_NCL, AVCOL_SPC_RESERVED, AVCOL_SPC_RGB,
    //};
    //static const enum AVColorRange color_ranges[] = {
    //    AVCOL_RANGE_MPEG, AVCOL_RANGE_JPEG
    //};
    //avctx->color_range = color_ranges[img->range];
    //avctx->colorspace = colorspaces[img->cs];
    switch (img->chroma_format) {
    case XVC_DEC_CHROMA_FORMAT_420:
        if (img->bitdepth == 8) {
            avctx->pix_fmt = AV_PIX_FMT_YUV420P;
        } else if (img->bitdepth == 10) {
            avctx->pix_fmt = AV_PIX_FMT_YUV420P10;
        } else if (img->bitdepth == 12) {
            avctx->pix_fmt = AV_PIX_FMT_YUV420P12;
        } else {
            return AVERROR_INVALIDDATA;
        }
        return 0;
    case XVC_DEC_CHROMA_FORMAT_422:
        if (img->bitdepth == 8) {
            avctx->pix_fmt = AV_PIX_FMT_YUV422P;
        } else if (img->bitdepth == 10) {
            avctx->pix_fmt = AV_PIX_FMT_YUV422P10;
        } else if (img->bitdepth == 12) {
            avctx->pix_fmt = AV_PIX_FMT_YUV422P12;
        } else {
            return AVERROR_INVALIDDATA;
        }
        return 0;
    case XVC_DEC_CHROMA_FORMAT_444:
        if (img->bitdepth == 8) {
            avctx->pix_fmt = AV_PIX_FMT_YUV440P;
        } else if (img->bitdepth == 10) {
            avctx->pix_fmt = AV_PIX_FMT_YUV440P10;
        } else if (img->bitdepth == 12) {
            avctx->pix_fmt = AV_PIX_FMT_YUV440P12;
        } else {
            return AVERROR_INVALIDDATA;
        }
        return 0;
    case XVC_DEC_CHROMA_FORMAT_ARGB:
        if (img->bitdepth == 8) {
            avctx->pix_fmt = AV_PIX_FMT_GBRP;
        } else if (img->bitdepth == 10) {
            avctx->pix_fmt = AV_PIX_FMT_GBRP10;
        } else if (img->bitdepth == 12) {
            avctx->pix_fmt = AV_PIX_FMT_GBRP12;
        } else {
            return AVERROR_INVALIDDATA;
        }
        return 0;
    default:
        return AVERROR_INVALIDDATA;
    }
}

static int xvc_decode_frame(AVCodecContext *avctx,
                            void *data, int *got_frame, AVPacket *avpkt)
{
    libxvcDecoderContext *ctx = avctx->priv_data;
    AVFrame *picture = data;
    xvc_dec_return_code xvc_ret;
    int ret;

    if (avpkt->data) {
        xvc_ret = ctx->api->decoder_decode_nal(ctx->decoder, avpkt->data,
                                               avpkt->size, avpkt->pts);
        if (xvc_ret != XVC_DEC_OK) {
            const char *error  = ctx->api->xvc_dec_get_error_text(xvc_ret);
            av_log(avctx, AV_LOG_ERROR, "Failed to decode nal: %s\n", error);
            return AVERROR_INVALIDDATA;
        }
    } else {
        xvc_ret = ctx->api->decoder_flush(ctx->decoder);
        if (xvc_ret != XVC_DEC_OK) {
            const char *error  = ctx->api->xvc_dec_get_error_text(xvc_ret);
            av_log(avctx, AV_LOG_ERROR, "Failed to flush decoder: %s\n", error);
            return AVERROR_INVALIDDATA;
        }
    }
    xvc_ret = ctx->api->decoder_get_picture(ctx->decoder, ctx->dec_pic);
    if (xvc_ret != XVC_DEC_OK && xvc_ret != XVC_DEC_NO_DECODED_PIC) {
        const char *error  = ctx->api->xvc_dec_get_error_text(xvc_ret);
        av_log(avctx, AV_LOG_ERROR, "Failed to get picture: %s\n", error);
        return AVERROR_INVALIDDATA;
    }
    if (xvc_ret == XVC_DEC_OK) {
        uint8_t *planes[4];
        int linesizes[4];
        int64_t pts;
        if ((ret = set_pix_fmt(avctx, &ctx->dec_pic->stats)) < 0) {
            av_log(avctx, AV_LOG_ERROR, "Unsupported output chroma format (%d) / bit_depth (%d)\n",
                   ctx->dec_pic->stats.chroma_format,
                   ctx->dec_pic->stats.bitdepth);
            return ret;
        }

        if (ctx->dec_pic->stats.width != avctx->width || ctx->dec_pic->stats.height != avctx->height) {
          av_log(avctx, AV_LOG_INFO, "dimension change! %dx%d -> %dx%d\n",
                 avctx->width, avctx->height, ctx->dec_pic->stats.width, ctx->dec_pic->stats.height);
          if ((ret = ff_set_dimensions(avctx, ctx->dec_pic->stats.width, ctx->dec_pic->stats.height)) < 0)
            return ret;
        }
        if ((ret = ff_get_buffer(avctx, picture, 0)) < 0)
            return ret;

        planes[0] = (uint8_t*) ctx->dec_pic->planes[0];
        planes[1] = (uint8_t*) ctx->dec_pic->planes[1];
        planes[2] = (uint8_t*) ctx->dec_pic->planes[2];
        planes[3] = NULL;
        linesizes[0] = ctx->dec_pic->stride[0];
        linesizes[1] = ctx->dec_pic->stride[1];
        linesizes[2] = ctx->dec_pic->stride[2];
        linesizes[3] = 0;
        av_image_copy(picture->data, picture->linesize, (const uint8_t**)planes,
                      linesizes, avctx->pix_fmt, ctx->dec_pic->stats.width,
                      ctx->dec_pic->stats.height);
        pts = ctx->dec_pic->user_data;
        if (pts == AV_NOPTS_VALUE) {
            pts = (avctx->pkt_timebase.den * ctx->dec_pic->stats.poc) /
                (ctx->dec_pic->stats.framerate * avctx->pkt_timebase.num);
        }
        picture->pts     = pts;
        picture->pkt_dts = pts;
#if FF_API_PKT_PTS
FF_DISABLE_DEPRECATION_WARNINGS
        picture->pkt_pts = pts;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
        *got_frame = 1;
    }

    return avpkt->size;
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

static av_cold void xvc_decode_init_csp(AVCodec *codec)
{
    codec->pix_fmts = xvc_csp_highbd;
}

#define OFFSET(x) offsetof(libxvcEncoderContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
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

AVCodec ff_libxvc_decoder = {
    .name           = "libxvc",
    .long_name      = NULL_IF_CONFIG_SMALL("libxvc xvc"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_XVC,
    .priv_data_size = sizeof(libxvcDecoderContext),
    .init           = xvc_init,
    .init_static_data = xvc_decode_init_csp,
    .close          = xvc_dec_close,
    .decode         = xvc_decode_frame,
    .priv_class     = &class,
    .defaults       = xvc_defaults,
    .capabilities   = AV_CODEC_CAP_AUTO_THREADS | AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_DELAY,
};
