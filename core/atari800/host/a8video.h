/* The Atari's picture as XRGB8888 (video.c). Emulation thread only. */
#ifndef A8VIDEO_H
#define A8VIDEO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { A8VIDEO_AREA_NARROW = 0, A8VIDEO_AREA_TV, A8VIDEO_AREA_FULL };

typedef struct {
	int area;           /* A8VIDEO_AREA_* */
} a8video_opts;

/* Renders the last frame into dst (maxw x maxh). Returns 0 if it would not
   fit. par_out: pixel aspect x1000 (always 1000). */
int a8video_render(uint32_t *dst, int maxw, int maxh, int *w_out, int *h_out,
                   int *par_out, const a8video_opts *o);
/* A point in the last rendered frame to Screen_atari coordinates. */
int a8video_to_screen(int fx, int fy, int *sx, int *sy);
uint32_t a8video_colour(int index);
void a8video_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif
