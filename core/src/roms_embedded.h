/* ROM images a developer build embeds (WITH_A5200_ROMS; never shipped). */
#ifndef A52_ROMS_EMBEDDED_H
#define A52_ROMS_EMBEDDED_H

#include <stdint.h>

typedef struct {
    const char *key;        /* the ROM slot key, e.g. "bios_4port" */
    const uint8_t *data;
    uint32_t size;
} a52_embedded_rom;

extern const a52_embedded_rom a52_embedded_roms[];
extern const unsigned a52_embedded_rom_count;

#endif
