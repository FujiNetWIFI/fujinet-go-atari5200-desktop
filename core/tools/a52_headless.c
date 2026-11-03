/* a52_headless: atari800 in 5200 mode with no window and no cartridge, for
   the boot and A/B tests (the FujiNet cartridge is core/cart's).

   a52_headless [options] [-- atari800 options]
     --frames N          run N frames (default 300)
     --hash              print one CRC32 over every frame's screen, and one
                         over all audio
     --expect-colours N  exit 1 unless the last frame shows at least N colours
     --log               print atari800's log

   Built twice: against the patched core (a52_headless) and the stock one
   (a52_headless_stock), from the same source. */
#include "config.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "akey.h"
#include "atari.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "screen.h"
#include "sound.h"

#include "a8host.h"

static uint32_t crc_table[256];

static void crc_init(void)
{
	uint32_t i, j, c;
	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
	const uint8_t *p = data;
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint32_t audio_crc;
static unsigned long audio_bytes;

static void on_sound(void *user, const void *pcm, unsigned bytes)
{
	(void)user;
	audio_crc = crc32_update(audio_crc, pcm, bytes);
	audio_bytes += bytes;
}

static void on_break(void *user)
{
	(void)user;
}

/* How many distinct colour indices the frame shows. */
static int distinct_colours(void)
{
	unsigned char seen[256] = {0};
	int n = 0;
	size_t i;
	for (i = 0; i < (size_t)Screen_WIDTH * Screen_HEIGHT; i++) {
		unsigned char c = ((const UBYTE *)Screen_atari)[i];
		if (!seen[c]) {
			seen[c] = 1;
			n++;
		}
	}
	return n;
}

int main(int argc, char **argv)
{
	int frames = 300, want_hash = 0, want_log = 0, want_colours = 0;
	char *a8argv[64];
	int a8argc = 0, i, f;
	uint32_t screen_crc = 0;
	a8host_callbacks cb = {0};
#ifdef A52_STOCK
	char cfg[FILENAME_MAX];
#endif

	a8argv[a8argc++] = "a52_headless";
	a8argv[a8argc++] = "-5200";
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--hash"))
			want_hash = 1;
		else if (!strcmp(argv[i], "--expect-colours") && i + 1 < argc)
			want_colours = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--log"))
			want_log = 1;
		else if (!strcmp(argv[i], "--")) {
			for (i++; i < argc && a8argc < 60; i++)
				a8argv[a8argc++] = argv[i];
		}
		else {
			fprintf(stderr, "a52_headless: unknown option %s\n", argv[i]);
			return 2;
		}
	}
#ifdef A52_STOCK
	/* stock atari800 reads and writes a config file: keep it out of $HOME */
	snprintf(cfg, sizeof cfg, "a52_headless_stock-%d.cfg", (int)getpid());
	{
		FILE *fp = fopen(cfg, "w");
		if (fp)
			fclose(fp);
	}
	a8argv[a8argc++] = "-config";
	a8argv[a8argc++] = cfg;
#else
	Atari800_host_paced = TRUE;
#endif
	a8argv[a8argc] = NULL;

	crc_init();
	cb.sound = on_sound;
	cb.brk = on_break;
	a8host_set_callbacks(&cb);
	Sound_desired.freq = A8HOST_AUDIO_RATE;
	Sound_latency = 0;

	if (!Atari800_Initialise(&a8argc, a8argv)) {
		fprintf(stderr, "a52_headless: Atari800_Initialise failed\n%s", Log_buffer);
		return 1;
	}
	for (f = 0; f < frames; f++) {
		INPUT_key_code = AKEY_NONE;
		Atari800_Frame();
		if (want_hash)
			screen_crc = crc32_update(screen_crc, Screen_atari, Screen_WIDTH * Screen_HEIGHT);
		if (want_log && Log_buffer[0]) {
			fputs(Log_buffer, stderr);
			Log_buffer[0] = 0;
		}
	}
	if (want_hash)
		printf("screen %08x audio %08x bytes %lu\n", screen_crc, audio_crc, audio_bytes);
#ifdef A52_STOCK
	remove(cfg);
#endif
	if (want_colours && distinct_colours() < want_colours) {
		fprintf(stderr, "a52_headless: only %d colours on screen, wanted %d\n",
		        distinct_colours(), want_colours);
		return 1;
	}
	return 0;
}
