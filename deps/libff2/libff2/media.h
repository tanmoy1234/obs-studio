#pragma once

#include <obs.h>
#include "decode.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <util/threading.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef void (*ff2_video_cb)(void *opaque, struct obs_source_frame *frame);
typedef void (*ff2_audio_cb)(void *opaque, struct obs_source_audio *audio);
typedef void (*ff2_stop_cb)(void *opaque);

struct ff2_media {
	AVFormatContext *fmt;

	void *opaque;
	ff2_video_cb v_cb;
	ff2_audio_cb a_cb;
	ff2_stop_cb stop_cb;

	struct SwsContext *swscale;
	uint8_t *scale_pic[4];
	int scale_linesizes[4];
	enum AVPixelFormat scale_format;

	struct ff2_decode v;
	struct ff2_decode a;
	bool has_video;
	bool has_audio;
	bool is_file;

	struct obs_source_frame obsframe;
	enum video_colorspace cur_space;
	enum video_range_type cur_range;

	int64_t base_ts;
	int64_t start_ts;
	int64_t next_pts_ns;
	uint64_t next_ns;

	os_sem_t *sem;
	pthread_mutex_t mutex;
	bool looping;
	bool stop;
	bool kill;

	pthread_t thread;
	bool thread_valid;
	volatile bool active;
};

typedef struct ff2_media ff2_media_t;

extern bool ff2_media_init(ff2_media_t *media, const char *path,
		void *opaque, ff2_video_cb v_cb, ff2_audio_cb a_cb,
		ff2_stop_cb stop_cb);
extern void ff2_media_free(ff2_media_t *media);

extern void ff2_media_play(ff2_media_t *media, bool loop);
extern void ff2_media_stop(ff2_media_t *media);

#ifdef __cplusplus
}
#endif
