/*
 * CD Graphics Video Decoder
 * Copyright (c) 2009 Michael Tison
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

#include "avcodec.h"
#include "bytestream.h"

/**
 * @file libavcodec/cdgraphics.c
 * @brief CD Graphics Video Decoder
 * @author Michael Tison
 * @sa http://wiki.multimedia.cx/index.php?title=CD_Graphics
 * @sa http://www.ccs.neu.edu/home/bchafy/cdb/info/cdg
 */

/// default screen sizes
#define CDG_FULL_WIDTH           300
#define CDG_FULL_HEIGHT          216
#define CDG_DISPLAY_WIDTH        294
#define CDG_DISPLAY_HEIGHT       204
#define CDG_BORDER_WIDTH           6
#define CDG_BORDER_HEIGHT         12

#define CDG_PALETTE_SIZE          16

/// masks
#define CDG_COMMAND             0x09
#define CDG_MASK                0x3F

/// instruction codes
#define CDG_INST_MEMORY_PRESET     1
#define CDG_INST_BORDER_PRESET     2
#define CDG_INST_TILE_BLOCK        6
#define CDG_INST_SCROLL_PRESET    20
#define CDG_INST_SCROLL_COPY      24
#define CDG_INST_LOAD_PAL_LO      30
#define CDG_INST_LOAD_PAL_HIGH    31
#define CDG_INST_TILE_BLOCK_XOR   38

/// data sizes
#define CDG_PACKET_SIZE           24
#define CDG_TILE_HEIGHT           12
#define CDG_TILE_WIDTH             6

typedef struct CdgPacket {
    uint8_t command;
    uint8_t instruction;
    uint8_t data[16];
} CdgPacket;

typedef struct CDGraphicsContext {
    AVFrame frame;
    int hscroll;
    int vscroll;
} CDGraphicsContext;

static void cdg_init_frame(AVFrame *frame)
{
    avcodec_get_frame_defaults(frame);
    frame->reference = 1;
    frame->buffer_hints = FF_BUFFER_HINTS_VALID    |
	                  FF_BUFFER_HINTS_PRESERVE |
	                  FF_BUFFER_HINTS_REUSABLE;
}

static av_cold int cdg_decode_init(AVCodecContext *avctx)
{
    CDGraphicsContext *cc = avctx->priv_data;

    cdg_init_frame(&cc->frame);

    avctx->width   = CDG_FULL_WIDTH;
    avctx->height  = CDG_FULL_HEIGHT;
    avctx->pix_fmt = PIX_FMT_PAL8;

    return 0;
}

static void cdg_get_preset_values(CdgPacket *cp, int *c, int *r)
{
    *c = cp->data[0] & 0x0F;
    *r = cp->data[1] & 0x0F;
}

static void cdg_memory_preset(CDGraphicsContext *cc, CdgPacket *cp)
{
    int color;
    int repeat;

    cdg_get_preset_values(cp, &color, &repeat);
    if (!repeat)
	memset(cc->frame.data[0], color,
	       cc->frame.linesize[0] * CDG_FULL_HEIGHT);
}

static void cdg_border_preset(CDGraphicsContext *cc, CdgPacket *cp)
{
    int color;
    int repeat;
    int y;
    int lsize    = cc->frame.linesize[0];
    uint8_t *buf = cc->frame.data[0];

    cdg_get_preset_values(cp, &color, &repeat);

    if (!repeat) {
	/// fill the top and bottom borders
	memset(buf, color, CDG_BORDER_HEIGHT * lsize);
	memset(buf + (CDG_FULL_HEIGHT - CDG_BORDER_HEIGHT) * lsize,
	       color, CDG_BORDER_HEIGHT * lsize);

	/// fill the side borders
	for (y = CDG_BORDER_HEIGHT;
	     y < CDG_FULL_HEIGHT - CDG_BORDER_HEIGHT; y++) {
	    memset(buf + y * lsize, color, CDG_BORDER_WIDTH);
	    memset(buf + CDG_FULL_WIDTH - CDG_BORDER_WIDTH + y * lsize,
		   color, CDG_BORDER_WIDTH);
	}
    }
}

static void cdg_load_palette(CDGraphicsContext *cc, CdgPacket *cp,
			     int low)
{
    uint8_t r, g, b;
    uint16_t color;
    int i;
    int array_offset  = low ? 0 : 8;
    uint32_t *palette = (uint32_t *) cc->frame.data[1];

    for (i = 0; i < 8; i++) {
	color = (cp->data[2 * i] << 6) + (cp->data[2 * i + 1] & 0x3F);
	r = (color >> 8) & 0x000F;
	g = (color >> 4) & 0x000F;
	b = (color     ) & 0x000F;
	r *= 17;
	g *= 17;
	b *= 17;
	palette[i + array_offset] = r << 16 | g << 8 | b;
    }
    cc->frame.data[1] = (uint8_t *) palette;
    cc->frame.palette_has_changed = 1;
}

static void cdg_tile_block(CDGraphicsContext *cc, CdgPacket *cp, int b)
{
    int c0, c1;
    int ci, ri;
    int byte, pix, color;
    int x, y;
    int ai;
    int lsize    = cc->frame.linesize[0];
    uint8_t *buf = cc->frame.data[0];

    c0 =  cp->data[0] & 0x0F;
    c1 =  cp->data[1] & 0x0F;
    ri = (cp->data[2] & 0x1F) * CDG_TILE_HEIGHT;
    ci = (cp->data[3] & 0x3F) * CDG_TILE_WIDTH;

    if (ri > (CDG_FULL_HEIGHT - CDG_TILE_HEIGHT - cc->vscroll)
	|| (ri + cc->vscroll) < 0)
	return;
    if (ci > (CDG_FULL_WIDTH - CDG_TILE_WIDTH - cc->hscroll)
	|| (ci + cc->hscroll) < 0)
	return;

    for (y = 0; y < CDG_TILE_HEIGHT; y++) {
	byte = cp->data[4 + y] & 0x3F;
	for (x = 0; x < CDG_TILE_WIDTH; x++) {
	    pix = (byte >> (5 - x)) & 0x01;

	    ai = ci + x + cc->hscroll + (lsize * (ri + y + cc->vscroll));

	    if (!pix)
		color = c0;
	    else
		color = c1;
	    if (b)
		color ^= buf[ai];
	    buf[ai] = color;
	}
    }
}

#define UP    2
#define DOWN  1
#define LEFT  2
#define RIGHT 1

static void cdg_get_scroll_data(CdgPacket *cp, int *color, int *hscmd,
				int *h_off, int *vscmd, int *v_off)
{
    int hscroll, vscroll;

    *color  = cp->data[0] & 0x0F;
    hscroll = cp->data[1] & 0x3F;
    vscroll = cp->data[2] & 0x3F;

    *hscmd = (hscroll & 0x30) >> 4;
    *h_off = (hscroll & 0x07);
    *vscmd = (vscroll & 0x30) >> 4;
    *v_off = (vscroll & 0x0F);

    *h_off = FFMIN(*h_off, CDG_BORDER_WIDTH  - 1);
    *v_off = FFMIN(*v_off, CDG_BORDER_HEIGHT - 1);
}

static void cdg_copy_rect_buf(int out_tl_x, int out_tl_y,
			      uint8_t *out,
			      int in_tl_x, int in_tl_y,
			      uint8_t *in, int w, int h, int lsize)
{
    int y;

    in  = in  + in_tl_x  + in_tl_y  * lsize;
    out = out + out_tl_x + out_tl_y * lsize;
    for (y = 0; y < h; y++)
	memcpy(out + y * lsize, in + y * lsize, w);
}

static void cdg_fill_rect_preset(int tl_x, int tl_y,
				 uint8_t *out, int color,
				 int w, int h, int lsize)
{
    int y;

    for (y = tl_y; y < tl_y + h; y++)
	memset(out + tl_x + y * lsize, color, w);
}

static void cdg_fill_wrapper(int out_tl_x, int out_tl_y,
			     uint8_t *out,
			     int in_tl_x, int in_tl_y,
			     uint8_t *in, int color,
			     int w, int h, int lsize, int roll)
{
    if (roll)
	cdg_copy_rect_buf(out_tl_x, out_tl_y, out, in_tl_x, in_tl_y,
			  in, w, h, lsize);
    else
	cdg_fill_rect_preset(out_tl_x, out_tl_y, out, color, w, h, lsize);
}

static void cdg_scroll(CDGraphicsContext *cc, CdgPacket *cp,
		       AVFrame *new_frame, int roll_over)
{
    int color;
    int hscmd, h_off, vscmd, v_off, dh_off, dv_off;
    int vinc = 0, hinc = 0, x, y;
    int lsize    = cc->frame.linesize[0];
    uint8_t *in  = cc->frame.data[0];
    uint8_t *out = new_frame->data[0];

    cdg_get_scroll_data(cp, &color, &hscmd, &h_off, &vscmd, &v_off);

    /// find the difference and save the offset for cdg_tile_block usage
    dh_off = h_off - cc->hscroll;
    dv_off = v_off - cc->vscroll;
    cc->hscroll = h_off;
    cc->vscroll = v_off;

    if (vscmd == UP)
	vinc = -12;
    if (vscmd == DOWN)
	vinc = 12;
    if (hscmd == LEFT)
	hinc = -6;
    if (hscmd == RIGHT)
	hinc = 6;
    vinc += dv_off;
    hinc += dh_off;

    if (!hinc && !vinc)
	return;

    memcpy(new_frame->data[1], cc->frame.data[1], CDG_PALETTE_SIZE * 4);

    for (y = FFMAX(0, vinc); y < FFMIN(CDG_FULL_HEIGHT + vinc, CDG_FULL_HEIGHT); y++)
	for (x = FFMAX(0, hinc); x < FFMIN(lsize + hinc, lsize); x++)
	    out[x + lsize * y] = in[x - hinc + (y - vinc) * lsize];

    if (vinc > 0)
	cdg_fill_wrapper(0, 0, out,
			 0, CDG_FULL_HEIGHT - vinc, in, color,
			 lsize, vinc, lsize, roll_over);
    else if (vinc < 0)
	cdg_fill_wrapper(0, CDG_FULL_HEIGHT + vinc, out,
			 0, 0, in, color,
			 lsize, -1 * vinc, lsize, roll_over);

    if (hinc > 0)
	cdg_fill_wrapper(0, 0, out,
			 CDG_FULL_WIDTH - hinc, 0, in, color,
			 hinc, CDG_FULL_HEIGHT, lsize, roll_over);
    else if (hinc < 0)
	cdg_fill_wrapper(CDG_FULL_WIDTH + hinc, 0, out,
			 0, 0, in, color,
			 -1 * hinc, CDG_FULL_HEIGHT, lsize, roll_over);

}

static int cdg_decode_frame(AVCodecContext *avctx,
			    void *data, int *data_size, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size = avpkt->size;
    AVFrame new_frame;
    CDGraphicsContext *cc = avctx->priv_data;
    CdgPacket cp;

    if (avctx->reget_buffer(avctx, &cc->frame)) {
	av_log(avctx, AV_LOG_ERROR, "reget_buffer() failed\n");
	return -1;
    }

    cp.command     = bytestream_get_byte(&buf);
    cp.instruction = bytestream_get_byte(&buf);
    buf += 2;  /// skipping 2 unneeded bytes
    bytestream_get_buffer(&buf, (uint8_t*) &cp.data, 16);

    if ((cp.command & CDG_MASK) == CDG_COMMAND) {
	switch (cp.instruction & CDG_MASK) {
	case CDG_INST_MEMORY_PRESET:
	    cdg_memory_preset(cc, &cp);
	    break;
	case CDG_INST_LOAD_PAL_LO:
	    cdg_load_palette(cc, &cp, 1);
	    break;
	case CDG_INST_LOAD_PAL_HIGH:
	    cdg_load_palette(cc, &cp, 0);
	    break;
	case CDG_INST_BORDER_PRESET:
	    cdg_border_preset(cc, &cp);
	    break;
	case CDG_INST_TILE_BLOCK:
	    cdg_tile_block(cc, &cp, 0);
	    break;
	case CDG_INST_TILE_BLOCK_XOR:
	    cdg_tile_block(cc, &cp, 1);
	    break;
	case CDG_INST_SCROLL_PRESET:
	case CDG_INST_SCROLL_COPY:
	    cdg_init_frame(&new_frame);
	    if (avctx->get_buffer(avctx, &new_frame)) {
		av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
		return -1;
	    }

	    if ((cp.instruction & CDG_MASK) == CDG_INST_SCROLL_PRESET)
		cdg_scroll(cc, &cp, &new_frame, 0);
	    else
		cdg_scroll(cc, &cp, &new_frame, 1);

	    avctx->release_buffer(avctx, &cc->frame);
	    cc->frame = new_frame;
	    break;
	default:
	    break;
	}

	*data_size = sizeof(AVFrame);
    } else {
	*data_size = 0;
	buf_size   = 0;
    }

    *(AVFrame *) data = cc->frame;
    return buf_size;
}

static av_cold int cdg_decode_end(AVCodecContext *avctx)
{
    CDGraphicsContext *cc = avctx->priv_data;

    if (cc->frame.data[0])
	avctx->release_buffer(avctx, &cc->frame);

    return 0;
}

AVCodec cdgraphics_decoder = {
    "cdgraphics",
    CODEC_TYPE_VIDEO,
    CODEC_ID_CDGRAPHICS,
    sizeof(CDGraphicsContext),
    cdg_decode_init,
    NULL,
    cdg_decode_end,
    cdg_decode_frame,
    CODEC_CAP_DR1,
    .long_name = NULL_IF_CONFIG_SMALL("CD Graphics video"),
};
