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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <util/circlebuf.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <util/threading.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

struct ff2_media;
typedef struct ff2_media ff2_media_t;

struct ff2_decode {
	ff2_media_t           *m;
	AVStream              *stream;
	bool                  audio;

	AVCodecContext        *decoder;
	AVCodec               *codec;

	int64_t               last_duration;
	int64_t               frame_pts;
	int64_t               next_pts;
	AVFrame               *frame;
	bool                  frame_ready;
	bool                  eof;

	AVPacket              orig_pkt;
	AVPacket              pkt;
	bool                  packet_pending;
	struct circlebuf      packets;
};

extern bool ff2_decode_init(ff2_media_t *media, enum AVMediaType type);
extern void ff2_decode_free(struct ff2_decode *decode);

extern void ff2_decode_clear_packets(struct ff2_decode *decode);

extern bool ff2_decode_push_packet(struct ff2_decode *decode, AVPacket *pkt);
extern bool ff2_decode_next(struct ff2_decode *decode);

#ifdef __cplusplus
}
#endif
