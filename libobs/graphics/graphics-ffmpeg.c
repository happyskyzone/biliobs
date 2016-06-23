#include "graphics.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include "../obs-ffmpeg-compat.h"

#define RESIZE_IMAGE

struct ffmpeg_image {
	const char         *file;
	AVFormatContext    *fmt_ctx;
	AVCodecContext     *decoder_ctx;
	AVCodec            *decoder;
	AVStream           *stream;
	int                stream_idx;

	int                cx, cy;
	enum AVPixelFormat format;
};


#if defined(RESIZE_IMAGE)

void ComputeDestSize(int sx, int sy, int* dx, int* dy)
{
	if (sx > 1920 || sy > 1080)
	{
		double srcRatio = (double)sx / sy;
		double outputRatio = 1920.0 / 1080;
		if (srcRatio > outputRatio)
		{
			*dx = 1920;
			*dy = (int)(1920 / srcRatio);
		}
		else
		{
			*dy = 1080;
			*dx = (int)(1080 * srcRatio);
		}
	}
	else
	{
		*dx = sx;
		*dy = sy;
	}
}

#endif


static bool ffmpeg_image_open_decoder_context(struct ffmpeg_image *info)
{
	int ret = av_find_best_stream(info->fmt_ctx, AVMEDIA_TYPE_VIDEO,
			-1, 1, NULL, 0);
	if (ret < 0) {
		blog(LOG_WARNING, "Couldn't find video stream in file '%s': %s",
				info->file, av_err2str(ret));
		return false;
	}

	info->stream_idx  = ret;
	info->stream      = info->fmt_ctx->streams[ret];
	info->decoder_ctx = info->stream->codec;
	info->decoder     = avcodec_find_decoder(info->decoder_ctx->codec_id);

	if (!info->decoder) {
		blog(LOG_WARNING, "Failed to find decoder for file '%s'",
				info->file);
		return false;
	}

	ret = avcodec_open2(info->decoder_ctx, info->decoder, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open video codec for file '%s': "
		                  "%s", info->file, av_err2str(ret));
		return false;
	}

	return true;
}

static void ffmpeg_image_free(struct ffmpeg_image *info)
{
	avcodec_close(info->decoder_ctx);
	avformat_close_input(&info->fmt_ctx);
}

static bool ffmpeg_image_init(struct ffmpeg_image *info, const char *file)
{
	int ret;

	if (!file || !*file)
		return false;

	memset(info, 0, sizeof(struct ffmpeg_image));
	info->file       = file;
	info->stream_idx = -1;

	ret = avformat_open_input(&info->fmt_ctx, file, NULL, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to open file '%s': %s",
				info->file, av_err2str(ret));
		return false;
	}

	ret = avformat_find_stream_info(info->fmt_ctx, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "Could not find stream info for file '%s':"
		                  " %s", info->file, av_err2str(ret));
		goto fail;
	}

	if (!ffmpeg_image_open_decoder_context(info))
		goto fail;

	info->cx     = info->decoder_ctx->width;
	info->cy     = info->decoder_ctx->height;
	info->format = info->decoder_ctx->pix_fmt;
	return true;

fail:
	ffmpeg_image_free(info);
	return false;
}

#if !defined(RESIZE_IMAGE)

static bool ffmpeg_image_reformat_frame(struct ffmpeg_image *info,
		AVFrame *frame, uint8_t *out, int linesize)
{
	struct SwsContext *sws_ctx = NULL;
	int               ret      = 0;

	if (info->format == AV_PIX_FMT_RGBA ||
	    info->format == AV_PIX_FMT_BGRA ||
	    info->format == AV_PIX_FMT_BGR0) {

		if (linesize != frame->linesize[0]) {
			int min_line = linesize < frame->linesize[0] ?
				linesize : frame->linesize[0];

			for (int y = 0; y < info->cy; y++)
				memcpy(out + y * linesize,
				       frame->data[0] + y * frame->linesize[0],
				       min_line);
		} else {
			memcpy(out, frame->data[0], linesize * info->cy);
		}

	} else {
		sws_ctx = sws_getContext(info->cx, info->cy, info->format,
				info->cx, info->cy, AV_PIX_FMT_BGRA,
				SWS_POINT, NULL, NULL, NULL);
		if (!sws_ctx) {
			blog(LOG_WARNING, "Failed to create scale context "
			                  "for '%s'", info->file);
			return false;
		}

		ret = sws_scale(sws_ctx, (const uint8_t *const*)frame->data,
				frame->linesize, 0, info->cy, &out, &linesize);
		sws_freeContext(sws_ctx);

		if (ret < 0) {
			blog(LOG_WARNING, "sws_scale failed for '%s': %s",
					info->file, av_err2str(ret));
			return false;
		}

		info->format = AV_PIX_FMT_BGRA;
	}

	return true;
}

#else

static bool ffmpeg_image_reformat_frame(struct ffmpeg_image *info,
	AVFrame *frame, uint8_t *out, int linesize)
{
	struct SwsContext *sws_ctx = NULL;
	int               ret = 0;

	int destX, destY;
	ComputeDestSize(info->cx, info->cy, &destX, &destY);

	sws_ctx = sws_getContext(info->cx, info->cy, info->format,
		destX, destY, AV_PIX_FMT_BGRA,
		SWS_POINT, NULL, NULL, NULL);
	if (!sws_ctx) {
		blog(LOG_WARNING, "Failed to create scale context "
			"for '%s'", info->file);
		return false;
	}

	ret = sws_scale(sws_ctx, (const uint8_t *const*)frame->data,
		frame->linesize, 0, info->cy, &out, &linesize);
	sws_freeContext(sws_ctx);

	if (ret < 0) {
		blog(LOG_WARNING, "sws_scale failed for '%s': %s",
			info->file, av_err2str(ret));
		return false;
	}

	info->format = AV_PIX_FMT_BGRA;

	return true;
}

#endif

static bool ffmpeg_image_decode(struct ffmpeg_image *info, uint8_t *out,
		int linesize)
{
	AVPacket          packet    = {0};
	bool              success   = false;
	AVFrame           *frame    = av_frame_alloc();
	int               got_frame = 0;
	int               ret;

	if (!frame) {
		blog(LOG_WARNING, "Failed to create frame data for '%s'",
				info->file);
		return false;
	}

	ret = av_read_frame(info->fmt_ctx, &packet);
	if (ret < 0) {
		blog(LOG_WARNING, "Failed to read image frame from '%s': %s",
				info->file, av_err2str(ret));
		goto fail;
	}

	while (!got_frame) {
		ret = avcodec_decode_video2(info->decoder_ctx, frame,
				&got_frame, &packet);
		if (ret < 0) {
			blog(LOG_WARNING, "Failed to decode frame for '%s': %s",
					info->file, av_err2str(ret));
			goto fail;
		}
	}

	success = ffmpeg_image_reformat_frame(info, frame, out, linesize);

fail:
	av_free_packet(&packet);
	av_frame_free(&frame);
	return success;
}

void gs_init_image_deps(void)
{
	av_register_all();
}

void gs_free_image_deps(void)
{
}

static inline enum gs_color_format convert_format(enum AVPixelFormat format)
{
	switch ((int)format) {
	case AV_PIX_FMT_RGBA: return GS_RGBA;
	case AV_PIX_FMT_BGRA: return GS_BGRA;
	case AV_PIX_FMT_BGR0: return GS_BGRX;
	}

	return GS_BGRX;
}



gs_texture_t *gs_texture_create_from_file(const char *file)
{
	struct ffmpeg_image image;
	gs_texture_t           *tex = NULL;

	if (ffmpeg_image_init(&image, file)) {
		uint8_t *data = malloc(image.cx * image.cy * 4);
		if (ffmpeg_image_decode(&image, data, image.cx * 4)) {
			tex = gs_texture_create(image.cx, image.cy,
					convert_format(image.format),
					1, (const uint8_t**)&data, 0);
		}

		ffmpeg_image_free(&image);
		free(data);
	}
	return tex;
}


gs_texture_t *gs_texture_create_from_file_resized(const char *file)
{
	struct ffmpeg_image image;
	gs_texture_t           *tex = NULL;

	if (ffmpeg_image_init(&image, file)) {
		uint8_t *data;
		int dx, dy;
		ComputeDestSize(image.cx, image.cy, &dx, &dy);

		data = malloc(dx * dy * 4);
		if (ffmpeg_image_decode(&image, data, dx * 4)) {
			tex = gs_texture_create(dx, dy,
				convert_format(image.format),
				1, (const uint8_t**)&data, 0);
		}

		ffmpeg_image_free(&image);
		free(data);
	}
	return tex;
}


uint8_t *gs_create_texture_file_data(const char *file,
	enum gs_color_format *format,
	uint32_t *cx_out, uint32_t *cy_out)
{
	struct ffmpeg_image image;
	uint8_t *data = NULL;

	if (ffmpeg_image_init(&image, file)) {
		data = bmalloc(image.cx * image.cy * 4);

		if (ffmpeg_image_decode(&image, data, image.cx * 4)) {
			*format = convert_format(image.format);
			*cx_out = (uint32_t)image.cx;
			*cy_out = (uint32_t)image.cy;
		}
		else {
			bfree(data);
			data = NULL;
		}

		ffmpeg_image_free(&image);
	}

	return data;
}


uint8_t *gs_create_texture_file_data_resized(const char *file,
	enum gs_color_format *format,
	uint32_t *cx_out, uint32_t *cy_out)
{
	struct ffmpeg_image image;
	int dx, dy;
	uint8_t *data = NULL;

	if (ffmpeg_image_init(&image, file)) {
		ComputeDestSize(image.cx, image.cy, &dx, &dy);
		data = bmalloc(dx * dy * 4);

		if (ffmpeg_image_decode(&image, data, dx * 4)) {
			*format = convert_format(image.format);
			image.cx = dx;
			image.cy = dy;
			*cx_out = (uint32_t)image.cx;
			*cy_out = (uint32_t)image.cy;
		}
		else {
			bfree(data);
			data = NULL;
		}

		ffmpeg_image_free(&image);
	}

	return data;
}
