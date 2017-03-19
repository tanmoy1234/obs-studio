/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "decode.h"
#include "media.h"

extern bool ff2_decode_init(ff2_media_t *m, enum AVMediaType type)
{
	struct ff2_decode *decode = type == AVMEDIA_TYPE_VIDEO ? &m->v : &m->a;
	AVStream *stream;
	int ret;

	memset(decode, 0, sizeof(*decode));
	decode->m = m;
	decode->audio = type == AVMEDIA_TYPE_AUDIO;

	ret = av_find_best_stream(m->fmt, type, -1, -1, NULL, 0);
	if (ret < 0)
		return false;
	stream = decode->stream = m->fmt->streams[ret];

	decode->codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!decode->codec) {
		blog(LOG_WARNING, "FF2: Failed to find %s codec",
				av_get_media_type_string(type));
		return false;
	}

	decode->decoder = avcodec_alloc_context3(decode->codec);
	if (!decode->decoder) {
		blog(LOG_WARNING, "FF2: Failed to allocate %s context",
				av_get_media_type_string(type));
		return false;
	}

	ret = avcodec_parameters_to_context(decode->decoder,
			decode->stream->codecpar);
	if (ret < 0) {
		blog(LOG_WARNING, "FF2: Failed to copy %s codec params: %s",
				av_get_media_type_string(type),
				av_err2str(ret));
		return false;
	}

	decode->decoder->thread_count = 0;

	ret = avcodec_open2(decode->decoder, decode->codec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "FF2: Failed to open %s decoder: %s",
				av_get_media_type_string(type),
				av_err2str(ret));
		return false;
	}

	decode->frame = av_frame_alloc();
	if (!decode->frame) {
		blog(LOG_WARNING, "FF2: Failed to allocate %s frame",
				av_get_media_type_string(type));
		return -1;
	}

	if (decode->codec->capabilities & CODEC_CAP_TRUNCATED)
		decode->decoder->flags |= CODEC_FLAG_TRUNCATED;
	return true;
}

void ff2_decode_clear_packets(struct ff2_decode *d)
{
	if (d->packet_pending) {
		av_packet_unref(&d->orig_pkt);
		d->packet_pending = false;
	}

	while (d->packets.size) {
		AVPacket pkt;
		circlebuf_pop_front(&d->packets, &pkt, sizeof(pkt));
		av_packet_unref(&pkt);
	}
}

void ff2_decode_free(struct ff2_decode *d)
{
	ff2_decode_clear_packets(d);
	circlebuf_free(&d->packets);

	if (d->decoder) {
		avcodec_close(d->decoder);
		av_free(d->decoder);
	}

	if (d->frame)
		av_free(d->frame);

	memset(d, 0, sizeof(*d));
}

bool ff2_decode_push_packet(struct ff2_decode *decode, AVPacket *packet)
{
	circlebuf_push_back(&decode->packets, packet, sizeof(*packet));
	return true;
}

static inline int64_t get_estimated_duration(struct ff2_decode *d,
		int64_t last_pts)
{
	if (last_pts)
		return d->frame_pts - last_pts;

	if (d->audio) {
		return av_rescale_q(d->frame->nb_samples,
				(AVRational){1, d->frame->sample_rate},
			 	(AVRational){1, 1000000000});
	} else {
		if (d->last_duration)
			return d->last_duration;

		return av_rescale_q(d->decoder->time_base.num,
				d->decoder->time_base,
				(AVRational){1, 1000000000});
	}
}	

bool ff2_decode_next(struct ff2_decode *d)
{
	int got_frame;
	int ret;

	d->frame_ready = false;

	if (!d->packets.size)
		return true;

	while (!d->frame_ready) {
		if (!d->packet_pending) {
			if (!d->packets.size)
				return true;

			circlebuf_pop_front(&d->packets, &d->orig_pkt,
					sizeof(d->orig_pkt));
			d->pkt = d->orig_pkt;
			d->packet_pending = true;
		}

		if (d->audio)
			ret = avcodec_decode_audio4(d->decoder,
					d->frame, &got_frame, &d->pkt);
		else
			ret = avcodec_decode_video2(d->decoder,
					d->frame, &got_frame, &d->pkt);
		if (ret < 0) {
			blog(LOG_WARNING, "FF2: decode failed: %s",
					av_err2str(ret));
			return false;
		}

		d->frame_ready = !!got_frame;

		d->pkt.data += ret;
		d->pkt.size -= ret;

		if (d->pkt.size == 0) {
			av_packet_unref(&d->orig_pkt);
			d->packet_pending = false;
		}
	}

	if (d->frame_ready) {
		int64_t last_pts = d->frame_pts;

		d->frame_pts = av_rescale_q(d->frame->pts,
				d->stream->time_base,
				(AVRational){1, 1000000000});

		int64_t duration = get_estimated_duration(d, last_pts);
		d->last_duration = duration;
		d->next_pts = d->frame_pts + duration;
	}

	return true;
}
