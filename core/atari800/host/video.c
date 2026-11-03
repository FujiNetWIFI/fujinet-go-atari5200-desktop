/* The Atari's picture as XRGB8888: the palette and the crop (after
   atari800's sdl/video_sw.c). The 5200 has no artifacting filters and no
   80-column cards. */
#include "config.h"
#include <string.h>

#include "a8video.h"
#include "atari.h"
#include "colours.h"
#include "screen.h"
#include "videomode.h"

static uint32_t lut[256];
/* the last render's crop, for a8video_to_screen */
static int last_x0, last_y0 = 8, last_w = 336;
static int pal_cache[256];
static int have_lut;

static void update_lut(void)
{
	int i;
	if (have_lut && !memcmp(pal_cache, Colours_table, sizeof pal_cache))
		return;
	memcpy(pal_cache, Colours_table, sizeof pal_cache);
	for (i = 0; i < 256; i++)
		lut[i] = 0xff000000u | (uint32_t)(Colours_table[i] & 0xffffff);
	have_lut = 1;
}

void a8video_invalidate(void)
{
	have_lut = 0;
}

uint32_t a8video_colour(int index)
{
	update_lut();
	return lut[index & 0xff];
}

int a8video_render(uint32_t *dst, int maxw, int maxh, int *w_out, int *h_out,
                   int *par_out, const a8video_opts *o)
{
	int x0, w, y0, h, x, y;

	update_lut();

	/* the Atari's own picture: 384x240 of colour indices, cropped */
	switch (o->area) {
	case A8VIDEO_AREA_NARROW:
		x0 = 32, w = 320, y0 = 20, h = 200;
		break;
	case A8VIDEO_AREA_FULL:
		x0 = 0, w = Screen_WIDTH, y0 = 0, h = Screen_HEIGHT;
		break;
	default:
		x0 = Screen_visible_x1, w = Screen_visible_x2 - Screen_visible_x1;
		if (Atari800_tv_mode == Atari800_TV_PAL)
			y0 = 0, h = Screen_HEIGHT;
		else
			y0 = 8, h = 224;
		break;
	}
	if (w > maxw || h > maxh)
		return 0;
	last_x0 = x0;
	last_y0 = y0;
	last_w = w;
	{
		const UBYTE *src = (const UBYTE *)Screen_atari + (size_t)y0 * Screen_WIDTH + x0;
		for (y = 0; y < h; y++) {
			const UBYTE *s = src + (size_t)y * Screen_WIDTH;
			uint32_t *d = dst + (size_t)y * w;
			for (x = 0; x < w; x++)
				d[x] = lut[s[x]];
		}
	}
	*par_out = 1000;
	*w_out = w;
	*h_out = h;
	return 1;
}

int a8video_to_screen(int fx, int fy, int *sx, int *sy)
{
	if (last_w <= 0)
		return 0;
	*sx = last_x0 + fx;
	*sy = last_y0 + fy;
	return 1;
}
