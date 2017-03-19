#include <obs.h>
#include <util/platform.h>

#include <assert.h>

#include "media.h"
#include "closest-format.h"

#include <libavutil/imgutils.h>

static inline bool active(const ff2_media_t *m)
{
	return os_atomic_load_bool(&m->active);
}

static inline enum video_format convert_pixel_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_NONE:    return VIDEO_FORMAT_NONE;
	case AV_PIX_FMT_YUV420P: return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:    return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUYV422: return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_UYVY422: return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_RGBA:    return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:    return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_BGR0:    return VIDEO_FORMAT_BGRX;
	default:;
	}

	return VIDEO_FORMAT_NONE;
}

static inline enum audio_format convert_sample_format(int f)
{
	switch (f) {
	case AV_SAMPLE_FMT_U8:   return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16:  return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:  return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT:  return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:  return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P: return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P: return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP: return AUDIO_FORMAT_FLOAT_PLANAR;
	default:;
	}

	return AUDIO_FORMAT_UNKNOWN;
}

static inline enum video_colorspace convert_color_space(enum AVColorSpace s)
{
	return s == AVCOL_SPC_BT709 ? VIDEO_CS_709 : VIDEO_CS_DEFAULT;
}

static inline enum video_range_type convert_color_range(enum AVColorRange r)
{
	return r == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL : VIDEO_RANGE_DEFAULT;
}

static inline struct ff2_decode *get_packet_decoder(ff2_media_t *media,
		AVPacket *pkt)
{
	if (media->has_audio &&
	    (!media->has_video || pkt->stream_index == media->a.stream->index))
		return &media->a;

	if (pkt->stream_index == media->v.stream->index)
		return &media->v;

	return NULL;
}

static int ff2_media_next_packet(ff2_media_t *media)
{
	AVPacket pkt;
	av_init_packet(&pkt);

	int ret = av_read_frame(media->fmt, &pkt);
	if (ret < 0) {
		if (ret != AVERROR_EOF)
			blog(LOG_WARNING, "FF2: av_read_frame failed: %d", ret);
		return ret;
	}

	struct ff2_decode *d = get_packet_decoder(media, &pkt);
	if (d && !ff2_decode_push_packet(d, &pkt)) {
		blog(LOG_WARNING, "FF2: failed to push packet");
		return -1;
	}
	if (!d)
		av_packet_unref(&pkt);

	return ret;
}

static bool ff2_media_seek_stream(ff2_media_t *m, struct ff2_decode *d,
		int64_t seek_pos, int seek_flags)
{
	int64_t seek_target = seek_flags == AVSEEK_FLAG_BACKWARD
		? av_rescale_q(seek_pos, AV_TIME_BASE_Q, d->stream->time_base)
		: seek_pos;

	int ret = av_seek_frame(m->fmt, d->stream->index, seek_target,
			seek_flags);
	if (ret < 0) {
		blog(LOG_WARNING, "FF2: Failed to seek");
		return false;
	}

	avcodec_flush_buffers(d->decoder);
	ff2_decode_clear_packets(d);
	d->frame_pts = 0;
	d->frame_ready = false;
	return true;
}

static inline bool ff2_media_ready_to_start(ff2_media_t *m)
{
	if (m->has_audio && !m->a.frame_ready)
		return false;
	if (m->has_video && !m->v.frame_ready)
		return false;
	return true;
}

static inline bool ff2_decode_frame(struct ff2_decode *d)
{
	return d->frame_ready || ff2_decode_next(d);
}

static bool ff2_media_prepare_frames(ff2_media_t *m)
{
	while (!ff2_media_ready_to_start(m)) {
		int ret = ff2_media_next_packet(m);
		if (ret == AVERROR_EOF)
			break;
		if (ret < 0)
			return false;

		if (m->has_video && !ff2_decode_frame(&m->v))
			return false;
		if (m->has_audio && !ff2_decode_frame(&m->a))
			return false;
	}

	return true;
}

static inline int64_t ff2_media_get_next_min_pts(ff2_media_t *m)
{
	int64_t min_next_ns = 0x7FFFFFFFFFFFFFFFLL;

	if (m->has_video && m->v.frame_ready) {
		if (m->v.frame_pts < min_next_ns)
			min_next_ns = m->v.frame_pts;
	}
	if (m->has_audio && m->a.frame_ready) {
		if (m->a.frame_pts < min_next_ns)
			min_next_ns = m->a.frame_pts;
	}

	return min_next_ns;
}

static bool ff2_media_reset(ff2_media_t *m)
{
	int64_t seek_pos;
	int seek_flags;

	if (m->fmt->duration == AV_NOPTS_VALUE) {
		seek_pos = 0;
		seek_flags = AVSEEK_FLAG_FRAME;
	} else {
		seek_pos = m->fmt->start_time;
		seek_flags = AVSEEK_FLAG_BACKWARD;
	}

	if (m->has_audio) {
		if (!ff2_media_seek_stream(m, &m->a, seek_pos, seek_flags)) {
			return false;
		}
	}
	if (m->has_video) {
		if (!ff2_media_seek_stream(m, &m->v, seek_pos, seek_flags)) {
			return false;
		}
	}

	if (!ff2_media_prepare_frames(m))
		return false;

	m->base_ts = m->next_pts_ns = ff2_media_get_next_min_pts(m);
	m->next_ns = 0;
	return true;
}

static inline bool ff2_media_can_play_frame(ff2_media_t *m,
		struct ff2_decode *d)
{
	return d->frame_ready && d->frame_pts <= m->next_pts_ns;
}

static void ff2_media_next_audio(ff2_media_t *m)
{
	struct ff2_decode *d = &m->a;
	struct obs_source_audio audio = {0};
	AVFrame *f = d->frame;

	if (!ff2_media_can_play_frame(m, d))
		return;

	for (size_t i = 0; i < MAX_AV_PLANES; i++)
		audio.data[i] = f->data[i];

	audio.samples_per_sec = f->sample_rate;
	audio.speakers = (enum speaker_layout)f->channels;
	audio.format = convert_sample_format(f->format);
	audio.frames = f->nb_samples;
	audio.timestamp = d->frame_pts;

	d->frame_ready = false;

	if (audio.format == AUDIO_FORMAT_UNKNOWN)
		return;

	m->a_cb(m->opaque, &audio);
}

static void ff2_media_next_video(ff2_media_t *m)
{
	struct ff2_decode *d = &m->v;
	struct obs_source_frame *frame = &m->obsframe;
	enum video_format new_format;
	enum video_colorspace new_space;
	enum video_range_type new_range;
	AVFrame *f = d->frame;

	if (!ff2_media_can_play_frame(m, d))
		return;

	if (m->swscale) {
		int ret = sws_scale(m->swscale,
				f->data, f->linesize,
				0, f->height,
				m->scale_pic, m->scale_linesizes);
		if (ret < 0)
			return;

		for (size_t i = 0; i < 4; i++) {
			frame->data[i] = m->scale_pic[i];
			frame->linesize[i] = m->scale_linesizes[i];
		}
	} else {
		for (size_t i = 0; i < MAX_AV_PLANES; i++) {
			frame->data[i] = f->data[i];
			frame->linesize[i] = f->linesize[i];
		}
	}

	new_format = convert_pixel_format(m->scale_format);
	new_space  = convert_color_space(f->colorspace);
	new_range  = convert_color_range(f->color_range);

	if (new_format != frame->format ||
	    new_space  != m->cur_space  ||
	    new_range  != m->cur_range) {
		bool success;

		frame->format = new_format;
		frame->full_range = new_range == VIDEO_RANGE_FULL;

		success = video_format_get_parameters(
				new_space,
				new_range,
				frame->color_matrix,
				frame->color_range_min,
				frame->color_range_max);

		frame->format = new_format;
		m->cur_space = new_space;
		m->cur_range = new_range;

		if (!success) {
			frame->format = VIDEO_FORMAT_NONE;
			return;
		}
	}

	d->frame_ready = false;

	if (frame->format == VIDEO_FORMAT_NONE)
		return;

	frame->timestamp = d->frame_pts;
	frame->width = f->width;
	frame->height = f->height;
	frame->flip = false;

	m->v_cb(m->opaque, frame);
}

static inline void ff2_media_sleepto(ff2_media_t *m)
{
	if (!m->next_ns)
		m->next_ns = os_gettime_ns();
	else
		os_sleepto_ns(m->next_ns);
}

static inline int64_t ff2_media_get_next_max_pts(ff2_media_t *m)
{
	int64_t max_next_ns = 0x8000000000000001LL;

	if (m->has_video && m->v.frame_ready) {
		if (m->v.frame_pts > max_next_ns)
			max_next_ns = m->v.frame_pts;
	}
	if (m->has_audio && m->a.frame_ready) {
		if (m->a.frame_pts > max_next_ns)
			max_next_ns = m->a.frame_pts;
	}

	return max_next_ns;
}

static inline void ff2_media_calc_next_ns(ff2_media_t *m)
{
	int64_t min_next_ns = ff2_media_get_next_min_pts(m);
	int64_t delta = min_next_ns - m->next_pts_ns;
	assert(delta > 0);
	m->next_ns += delta;
	m->next_pts_ns = min_next_ns;
}

static inline bool ff2_media_eof(ff2_media_t *m)
{
	bool v_ended = !m->has_video || !m->v.frame_ready;
	bool a_ended = !m->has_audio || !m->a.frame_ready;
	bool eof = v_ended && a_ended;

	if (eof) {
		os_atomic_set_bool(&m->active, false);
		ff2_media_reset(m);
		m->stop_cb(m->opaque);
	}

	return eof;
}

static void *ff2_media_thread(void *opaque)
{
	ff2_media_t *m = opaque;

	os_set_thread_name("ff2_media_thread");

	ff2_media_reset(m);

	for (;;) {
		bool stop, kill;
		bool is_active = active(m);

		if (!is_active) {
			if (os_sem_wait(m->sem) < 0)
				return NULL;
		} else {
			ff2_media_sleepto(m);
		}

		pthread_mutex_lock(&m->mutex);

		stop = m->stop;
		kill = m->kill;
		m->stop = false;
		m->kill = false;

		pthread_mutex_unlock(&m->mutex);

		if (kill) {
			break;
		}
		if (stop) {
			ff2_media_reset(m);
			continue;
		}

		/* frames are ready */
		if (is_active) {
			if (m->has_video)
				ff2_media_next_video(m);
			if (m->has_audio)
				ff2_media_next_audio(m);

			if (!ff2_media_prepare_frames(m))
				return NULL;
			if (ff2_media_eof(m))
				continue;

			ff2_media_calc_next_ns(m);
		}
	}

	return NULL;
}

static inline int get_sws_colorspace(enum AVColorSpace cs)
{
	switch (cs) {
	case AVCOL_SPC_BT709:
		return SWS_CS_ITU709;
	case AVCOL_SPC_FCC:
		return SWS_CS_FCC;
	case AVCOL_SPC_SMPTE170M:
		return SWS_CS_SMPTE170M;
	case AVCOL_SPC_SMPTE240M:
		return SWS_CS_SMPTE240M;
	case AVCOL_SPC_BT2020_NCL:
	case AVCOL_SPC_BT2020_CL:
		return SWS_CS_BT2020;
	default:
		break;
	}

	return SWS_CS_ITU601;
}

static inline int get_sws_range(enum AVColorRange r)
{
	return r == AVCOL_RANGE_JPEG ? 1 : 0;
}

#define FIXED_1_0 (1<<16)

static bool ff2_media_init_scaling(ff2_media_t *m)
{
	int space = get_sws_colorspace(m->v.decoder->colorspace);
	int range = get_sws_range(m->v.decoder->color_range);
	const int *coeff = sws_getCoefficients(space);

	m->swscale = sws_getCachedContext(NULL,
			m->v.decoder->width, m->v.decoder->height,
			m->v.decoder->pix_fmt,
			m->v.decoder->width, m->v.decoder->height,
			m->scale_format,
			SWS_FAST_BILINEAR, NULL, NULL, NULL);
	if (!m->swscale) {
		blog(LOG_WARNING, "FF2: Failed to initialize scaler");
		return false;
	}

	sws_setColorspaceDetails(m->swscale, coeff, range, coeff, range, 0,
			FIXED_1_0, FIXED_1_0);

	int ret = av_image_alloc(m->scale_pic, m->scale_linesizes,
			m->v.decoder->width, m->v.decoder->height,
			m->scale_format, 1);
	if (ret < 0) {
		blog(LOG_WARNING, "FF2: Failed to create scale pic data");
		return false;
	}

	return true;
}

static inline bool ff2_media_init_internal(ff2_media_t *m, const char *path)
{
	if (pthread_mutex_init(&m->mutex, NULL) != 0) {
		blog(LOG_WARNING, "FF2: Failed to init mutex");
		return false;
	}
	if (os_sem_init(&m->sem, 0) != 0) {
		blog(LOG_WARNING, "FF2: Failed to init semaphore");
		return false;
	}

	int ret = avformat_open_input(&m->fmt, path, NULL, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "FF2: Failed to open media: '%s'", path);
		return false;
	}

	if (avformat_find_stream_info(m->fmt, NULL) < 0) {
		blog(LOG_WARNING, "FF2: Failed to find stream info for '%s'",
				path);
		return false;
	}

	m->has_video = ff2_decode_init(m, AVMEDIA_TYPE_VIDEO);
	m->has_audio = ff2_decode_init(m, AVMEDIA_TYPE_AUDIO);

	if (!m->has_video && !m->has_audio) {
		blog(LOG_WARNING, "FF2: Could not initialize audio or video: "
				"'%s'", path);
		return false;
	}

	if (m->has_video) {
		m->scale_format = closest_format(m->v.decoder->pix_fmt);
		if (m->scale_format != m->v.decoder->pix_fmt) {
			if (!ff2_media_init_scaling(m)) {
				return false;
			}
		}
	}

	if (pthread_create(&m->thread, NULL, ff2_media_thread, m) != 0) {
		blog(LOG_WARNING, "FF2: Could not create media thread");
		return false;
	}

	m->thread_valid = true;
	return true;
}

bool ff2_media_init(ff2_media_t *media, const char *path,
		void *opaque, ff2_video_cb v_cb, ff2_audio_cb a_cb,
		ff2_stop_cb stop_cb)
{
	memset(media, 0, sizeof(*media));
	pthread_mutex_init_value(&media->mutex);
	media->opaque = opaque;
	media->v_cb = v_cb;
	media->a_cb = a_cb;
	media->stop_cb = stop_cb;

	if (!ff2_media_init_internal(media, path)) {
		ff2_media_free(media);
		return false;
	}

	return true;
}

static void ff2_kill_thread(ff2_media_t *m)
{
	if (m->thread_valid) {
		pthread_mutex_lock(&m->mutex);
		m->kill = true;
		pthread_mutex_unlock(&m->mutex);
		os_sem_post(m->sem);

		pthread_join(m->thread, NULL);
	}
}

void ff2_media_free(ff2_media_t *media)
{
	if (!media)
		return;

	ff2_media_stop(media);
	ff2_kill_thread(media);
	ff2_decode_free(&media->v);
	ff2_decode_free(&media->a);
	pthread_mutex_destroy(&media->mutex);
	os_sem_destroy(media->sem);
	avformat_close_input(&media->fmt);
	sws_freeContext(media->swscale);
	av_freep(&media->scale_pic[0]);
	memset(media, 0, sizeof(*media));
}

void ff2_media_play(ff2_media_t *m, bool loop)
{
	if (!active(m)) {
		os_atomic_set_bool(&m->active, true);
	} else {
		pthread_mutex_lock(&m->mutex);
		m->stop = true;
		os_atomic_set_bool(&m->active, true);
		pthread_mutex_unlock(&m->mutex);
	}
	os_sem_post(m->sem);		
}

void ff2_media_stop(ff2_media_t *m)
{
	if (active(m)) {
		pthread_mutex_lock(&m->mutex);
		m->stop = true;
		os_atomic_set_bool(&m->active, false);
		pthread_mutex_unlock(&m->mutex);

		os_sem_post(m->sem);		
	}
}
