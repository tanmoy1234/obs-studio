/*
 * Copyright (c) 2015 John R. Bradley <jrb@turrettech.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
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

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>

#include "obs-ffmpeg-compat.h"
#include "obs-ffmpeg-formats.h"

#include <libff2/media.h>

#include <libswscale/swscale.h>

#define FF_LOG(level, format, ...) \
	blog(level, "[Media Source]: " format, ##__VA_ARGS__)
#define FF_LOG_S(source, level, format, ...) \
	blog(level, "[Media Source '%s']: " format, \
			obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) \
	FF_LOG_S(s->source, level, format, ##__VA_ARGS__)

static bool video_frame(struct ff_frame *frame, void *opaque);
static bool video_format(AVCodecContext *codec_context, void *opaque);

struct ffmpeg_source {
	ff2_media_t media;
	bool media_valid;

	struct SwsContext *sws_ctx;
	int sws_width;
	int sws_height;
	enum AVPixelFormat sws_format;
	uint8_t *sws_data;
	int sws_linesize;
	obs_source_t *source;

	char *input;
	char *input_format;
	enum AVDiscard frame_drop;
	enum video_range_type range;
	int audio_buffer_size;
	int video_buffer_size;
	bool is_advanced;
	bool is_looping;
	bool is_forcing_scale;
	bool is_hw_decoding;
	bool is_clear_on_media_end;
	bool restart_on_activate;
};

static bool is_local_file_modified(obs_properties_t *props,
		obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "is_local_file");
	obs_property_t *input = obs_properties_get(props, "input");
	obs_property_t *input_format =obs_properties_get(props,
			"input_format");
	obs_property_t *local_file = obs_properties_get(props, "local_file");
	obs_property_t *looping = obs_properties_get(props, "looping");
	obs_property_set_visible(input, !enabled);
	obs_property_set_visible(input_format, !enabled);
	obs_property_set_visible(local_file, enabled);
	obs_property_set_visible(looping, enabled);

	return true;
}

static bool is_advanced_modified(obs_properties_t *props,
		obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "advanced");
	obs_property_t *fscale = obs_properties_get(props, "force_scale");
	obs_property_t *abuf = obs_properties_get(props, "audio_buffer_size");
	obs_property_t *vbuf = obs_properties_get(props, "video_buffer_size");
	obs_property_t *frame_drop = obs_properties_get(props, "frame_drop");
	obs_property_t *color_range = obs_properties_get(props, "color_range");
	obs_property_set_visible(fscale, enabled);
	obs_property_set_visible(abuf, enabled);
	obs_property_set_visible(vbuf, enabled);
	obs_property_set_visible(frame_drop, enabled);
	obs_property_set_visible(color_range, enabled);

	return true;
}

static void ffmpeg_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "is_local_file", true);
	obs_data_set_default_bool(settings, "looping", false);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
	obs_data_set_default_bool(settings, "force_scale", true);
#if defined(_WIN32)
	obs_data_set_default_bool(settings, "hw_decode", true);
#endif
}

static const char *media_filter =
	" (*.mp4 *.ts *.mov *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter =
	" (*.mp4 *.ts *.mov *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter =
	" (*.mp3 *.aac *.ogg *.wav);;";

static obs_properties_t *ffmpeg_source_getproperties(void *data)
{
	struct ffmpeg_source *s = data;
	struct dstr filter = {0};
	struct dstr path = {0};
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop;
	// use this when obs allows non-readonly paths
	prop = obs_properties_add_bool(props, "is_local_file",
			obs_module_text("LocalFile"));

	obs_property_set_modified_callback(prop, is_local_file_modified);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	if (s && s->input && *s->input) {
		const char *slash;

		dstr_copy(&path, s->input);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
	}

	obs_properties_add_path(props, "local_file",
			obs_module_text("LocalFile"), OBS_PATH_FILE,
			filter.array, path.array);
	dstr_free(&filter);
	dstr_free(&path);

	obs_properties_add_bool(props, "looping", obs_module_text("Looping"));

	obs_properties_add_bool(props, "restart_on_activate",
			obs_module_text("RestartWhenActivated"));

	obs_properties_add_text(props, "input",
			obs_module_text("Input"), OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "input_format",
			obs_module_text("InputFormat"), OBS_TEXT_DEFAULT);

	obs_properties_add_bool(props, "hw_decode",
			obs_module_text("HardwareDecode"));

	obs_properties_add_bool(props, "clear_on_media_end",
			obs_module_text("ClearOnMediaEnd"));

	prop = obs_properties_add_bool(props, "advanced",
			obs_module_text("Advanced"));

	obs_property_set_modified_callback(prop, is_advanced_modified);

	obs_properties_add_bool(props, "force_scale",
			obs_module_text("ForceFormat"));

	prop = obs_properties_add_int(props, "audio_buffer_size",
			obs_module_text("AudioBufferSize"), 1, 9999, 1);

	obs_property_set_visible(prop, false);

	prop = obs_properties_add_int(props, "video_buffer_size",
			obs_module_text("VideoBufferSize"), 1, 9999, 1);

	obs_property_set_visible(prop, false);

	prop = obs_properties_add_list(props, "frame_drop",
			obs_module_text("FrameDropping"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(prop, obs_module_text("DiscardNone"),
			AVDISCARD_NONE);
	obs_property_list_add_int(prop, obs_module_text("DiscardDefault"),
			AVDISCARD_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("DiscardNonRef"),
			AVDISCARD_NONREF);
	obs_property_list_add_int(prop, obs_module_text("DiscardBiDir"),
			AVDISCARD_BIDIR);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 67, 100)
	obs_property_list_add_int(prop, obs_module_text("DiscardNonIntra"),
			AVDISCARD_NONINTRA);
#endif
	obs_property_list_add_int(prop, obs_module_text("DiscardNonKey"),
			AVDISCARD_NONKEY);
	obs_property_list_add_int(prop, obs_module_text("DiscardAll"),
			AVDISCARD_ALL);

	obs_property_set_visible(prop, false);

	prop = obs_properties_add_list(props, "color_range",
			obs_module_text("ColorRange"), OBS_COMBO_TYPE_LIST,
			OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Auto"),
			VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Partial"),
			VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Full"),
			VIDEO_RANGE_FULL);

	obs_property_set_visible(prop, false);

	return props;
}

static const char *frame_drop_to_str(enum AVDiscard discard)
{
#define DISCARD_CASE(x) case AVDISCARD_ ## x: return "AVDISCARD_" #x
	switch (discard)
	{
	DISCARD_CASE(NONE);
	DISCARD_CASE(DEFAULT);
	DISCARD_CASE(NONREF);
	DISCARD_CASE(BIDIR);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 67, 100)
	DISCARD_CASE(NONINTRA);
#endif
	DISCARD_CASE(NONKEY);
	DISCARD_CASE(ALL);
	default: return "(Unknown)";
	};
#undef DISCARD_CASE
}

static void dump_source_info(struct ffmpeg_source *s, const char *input,
		const char *input_format, bool is_advanced)
{
	FF_BLOG(LOG_INFO,
			"settings:\n"
			"\tinput:                   %s\n"
			"\tinput_format:            %s\n"
			"\tis_looping:              %s\n"
			"\tis_forcing_scale:        %s\n"
			"\tis_hw_decoding:          %s\n"
			"\tis_clear_on_media_end:   %s\n"
			"\trestart_on_activate:     %s",
			input ? input : "(null)",
			input_format ? input_format : "(null)",
			s->is_looping ? "yes" : "no",
			s->is_forcing_scale ? "yes" : "no",
			s->is_hw_decoding ? "yes" : "no",
			s->is_clear_on_media_end ? "yes" : "no",
			s->restart_on_activate ? "yes" : "no");

	if (!is_advanced)
		return;

	FF_BLOG(LOG_INFO,
			"advanced settings:\n"
			"\taudio_buffer_size:       %d\n"
			"\tvideo_buffer_size:       %d\n"
			"\tframe_drop:              %s",
			s->audio_buffer_size,
			s->video_buffer_size,
			frame_drop_to_str(s->frame_drop));
}

static void get_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_source *s = opaque;
	obs_source_output_video(s->source, f);
}

static void get_audio(void *opaque, struct obs_source_audio *a)
{
	struct ffmpeg_source *s = opaque;
	obs_source_output_audio(s->source, a);
}

static void media_stopped(void *opaque)
{
	struct ffmpeg_source *s = opaque;
	if (s->is_clear_on_media_end)
		obs_source_output_video(s->source, NULL);
}

static void ffmpeg_source_start(struct ffmpeg_source *s)
{
	if (s->media_valid)
		ff2_media_play(&s->media, false);
}

static void ffmpeg_source_update(void *data, obs_data_t *settings)
{
	struct ffmpeg_source *s = data;

	bool is_local_file = obs_data_get_bool(settings, "is_local_file");
	bool is_advanced = obs_data_get_bool(settings, "advanced");

	char *input;
	char *input_format;

	bfree(s->input);
	bfree(s->input_format);

	if (is_local_file) {
		input = (char *)obs_data_get_string(settings, "local_file");
		input_format = NULL;
		s->is_looping = obs_data_get_bool(settings, "looping");
	} else {
		input = (char *)obs_data_get_string(settings, "input");
		input_format = (char *)obs_data_get_string(settings,
				"input_format");
		s->is_looping = false;
	}

	s->input = input ? bstrdup(input) : NULL;
	s->input_format = input_format ? bstrdup(input_format) : NULL;
	s->is_advanced = is_advanced;
	s->is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
	s->is_clear_on_media_end = obs_data_get_bool(settings,
			"clear_on_media_end");
	s->restart_on_activate = obs_data_get_bool(settings,
			"restart_on_activate");
	s->is_forcing_scale = true;
	s->range = VIDEO_RANGE_DEFAULT;

	if (is_advanced) {
		s->audio_buffer_size = (int)obs_data_get_int(settings,
				"audio_buffer_size");
		s->video_buffer_size = (int)obs_data_get_int(settings,
				"video_buffer_size");
		s->frame_drop = (enum AVDiscard)obs_data_get_int(settings,
					"frame_drop");
		s->is_forcing_scale = obs_data_get_bool(settings,
				"force_scale");
		s->range = (enum video_range_type)obs_data_get_int(settings,
				"color_range");

		if (s->audio_buffer_size < 1) {
			s->audio_buffer_size = 1;
			FF_BLOG(LOG_WARNING, "invalid audio_buffer_size %d",
					s->audio_buffer_size);
		}
		if (s->video_buffer_size < 1) {
			s->video_buffer_size = 1;
			FF_BLOG(LOG_WARNING, "invalid audio_buffer_size %d",
					s->audio_buffer_size);
		}

		if (s->frame_drop < AVDISCARD_NONE ||
		    s->frame_drop > AVDISCARD_ALL) {
			s->frame_drop = AVDISCARD_DEFAULT;
			FF_BLOG(LOG_WARNING, "invalid frame_drop %d",
					s->frame_drop);
		}
	}

	if (s->media_valid) {
		ff2_media_free(&s->media);
		s->media_valid = false;
	}

	if (s->input && *s->input)
		s->media_valid = ff2_media_init(&s->media, s->input,
				s, get_frame, get_audio, media_stopped);

	dump_source_info(s, input, input_format, is_advanced);
	if (!s->restart_on_activate || obs_source_active(s->source))
		ffmpeg_source_start(s);
}

static const char *ffmpeg_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFMpegSource");
}

static void *ffmpeg_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);

	struct ffmpeg_source *s = bzalloc(sizeof(struct ffmpeg_source));
	s->source = source;

	ffmpeg_source_update(s, settings);
	return s;
}

static void ffmpeg_source_destroy(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->media_valid)
		ff2_media_free(&s->media);

	if (s->sws_ctx != NULL)
		sws_freeContext(s->sws_ctx);
	bfree(s->sws_data);
	bfree(s->input);
	bfree(s->input_format);
	bfree(s);
}

static void ffmpeg_source_activate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate)
		ffmpeg_source_start(s);
}

static void ffmpeg_source_deactivate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate) {
		if (s->media_valid) {
			ff2_media_stop(&s->media);

			if (s->is_clear_on_media_end)
				obs_source_output_video(s->source, NULL);
		}
	}
}

struct obs_source_info ffmpeg_source = {
	.id             = "ffmpeg_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
	                  OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = ffmpeg_source_getname,
	.create         = ffmpeg_source_create,
	.destroy        = ffmpeg_source_destroy,
	.get_defaults   = ffmpeg_source_defaults,
	.get_properties = ffmpeg_source_getproperties,
	.activate       = ffmpeg_source_activate,
	.deactivate     = ffmpeg_source_deactivate,
	.update         = ffmpeg_source_update
};
