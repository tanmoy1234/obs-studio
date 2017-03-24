/*
 * Copyright (c) 2017 Hugh Bailey <obs.jim@gmail.com>
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

#include "decode.h"
#include "media.h"

extern bool ff2_decode_init(ff2_media_t *m, enum AVMediaType type)
{
	struct ff2_decode *d = type == AVMEDIA_TYPE_VIDEO ? &m->v : &m->a;
	AVStream *stream;
	int ret;

	memset(d, 0, sizeof(*d));
	d->m = m;
	d->audio = type == AVMEDIA_TYPE_AUDIO;

	ret = av_find_best_stream(m->fmt, type, -1, -1, NULL, 0);
	if (ret < 0)
		return false;
	stream = d->stream = m->fmt->streams[ret];

	if (stream->codecpar->codec_id == AV_CODEC_ID_VP8)
		d->codec = avcodec_find_decoder_by_name("libvpx");
	else if (stream->codecpar->codec_id == AV_CODEC_ID_VP9)
		d->codec = avcodec_find_decoder_by_name("libvpx-vp9");

	if (!d->codec)
		d->codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!d->codec) {
		blog(LOG_WARNING, "FF2: Failed to find %s codec",
				av_get_media_type_string(type));
		return false;
	}

	d->decoder = avcodec_alloc_context3(d->codec);
	if (!d->decoder) {
		blog(LOG_WARNING, "FF2: Failed to allocate %s context",
				av_get_media_type_string(type));
		return false;
	}

	ret = avcodec_parameters_to_context(d->decoder,
			d->stream->codecpar);
	if (ret < 0) {
		blog(LOG_WARNING, "FF2: Failed to copy %s codec params: %s",
				av_get_media_type_string(type),
				av_err2str(ret));
		return false;
	}

	if (d->decoder->thread_count == 1 &&
	    d->decoder->codec_id != AV_CODEC_ID_PNG &&
	    d->decoder->codec_id != AV_CODEC_ID_TIFF &&
	    d->decoder->codec_id != AV_CODEC_ID_JPEG2000 &&
	    d->decoder->codec_id != AV_CODEC_ID_MPEG4 &&
	    d->decoder->codec_id != AV_CODEC_ID_WEBP)
		d->decoder->thread_count = 0;

	ret = avcodec_open2(d->decoder, d->codec, NULL);
	if (ret < 0) {
		blog(LOG_WARNING, "FF2: Failed to open %s decoder: %s",
				av_get_media_type_string(type),
				av_err2str(ret));
		return false;
	}

	d->frame = av_frame_alloc();
	if (!d->frame) {
		blog(LOG_WARNING, "FF2: Failed to allocate %s frame",
				av_get_media_type_string(type));
		return -1;
	}

	if (d->codec->capabilities & CODEC_CAP_TRUNCATED)
		d->decoder->flags |= CODEC_FLAG_TRUNCATED;
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
	bool eof = d->m->eof;
	int got_frame;
	int ret;

	d->frame_ready = false;

	if (!eof && !d->packets.size)
		return true;

	while (!d->frame_ready) {
		if (!d->packet_pending) {
			if (!d->packets.size) {
				if (eof) {
					d->pkt.data = NULL;
					d->pkt.size = 0;
				} else {
					return true;
				}
			} else {
				circlebuf_pop_front(&d->packets, &d->orig_pkt,
						sizeof(d->orig_pkt));
				d->pkt = d->orig_pkt;
				d->packet_pending = true;
			}
		}

		if (d->audio)
			ret = avcodec_decode_audio4(d->decoder,
					d->frame, &got_frame, &d->pkt);
		else
			ret = avcodec_decode_video2(d->decoder,
					d->frame, &got_frame, &d->pkt);
		if (!got_frame && ret == 0) {
			d->eof = true;
			return true;
		}
		if (ret < 0) {
			blog(LOG_WARNING, "FF2: decode failed: %s",
					av_err2str(ret));
			return false;
		}

		d->frame_ready = !!got_frame;

		if (d->packet_pending) {
			if (d->pkt.size) {
				d->pkt.data += ret;
				d->pkt.size -= ret;
			}

			if (d->pkt.size == 0) {
				av_packet_unref(&d->orig_pkt);
				av_init_packet(&d->orig_pkt);
				av_init_packet(&d->pkt);
				d->packet_pending = false;
			}
		}
	}

	if (d->frame_ready) {
		int64_t last_pts = d->frame_pts;

		d->frame_pts = av_rescale_q(d->frame->best_effort_timestamp,
				d->stream->time_base,
				(AVRational){1, 1000000000});

		int64_t duration = d->frame->pkt_duration;
		if (!duration)
			duration = get_estimated_duration(d, last_pts);
		else
			duration = av_rescale_q(duration,
					d->stream->time_base,
					(AVRational){1, 1000000000});
		d->last_duration = duration;
		d->next_pts = d->frame_pts + duration;
	}

	return true;
}
