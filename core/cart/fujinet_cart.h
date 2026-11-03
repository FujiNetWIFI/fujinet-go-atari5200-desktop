/* fujinet_cart.h -- the FujiNet Atari 5200 cartridge, the one cart with a
 * network behind it.
 *
 * A C transposition of the firmware's own MAME device (fujinet-firmware
 * pico/atari-5200/emu/a5200_fujinet.cpp, BSD-3-Clause, Thomas Cherryhomes):
 * an RP2354B on the cart edge (A0-A13, D0-D7, two enables, no R/W) that serves
 * the console's reads from a 32K view and decodes the mailbox hotspots the
 * console READS at $B500-$B7FF. The mapper engine (a52map.c: ROM, 2 chips,
 * Bounty Bob, Super Cart <= 128K), the staging (fuji_load.c), the .cfg
 * (fuji_cfg.c), the mailbox protocol (fujimail.c) and the wire codec
 * (fujibus.c) are the cartridge firmware's own sources, staged verbatim into
 * fuji-generated/ by cmake/StageFujiProto.cmake; this file is only the port.
 *
 * Like the cartridge, the mailbox service runs on its own thread (core0) while
 * the bus is served on the emulation thread (core1); sync mode runs it inline
 * (deterministic: the tests). The link reconnects: a dropped BoIP socket is
 * dialled again before the next transaction.
 *
 * The cart has no R/W, so a write is a cycle like a read (same side effects).
 * There is no reset line: power_on is the only way back to CONFIG.
 *
 * Single instance per process (fujimail's port callbacks carry no context).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52_FUJINET_CART_H
#define A52_FUJINET_CART_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a52_fujinet a52_fujinet_t;

/* Create the (single) cart; NULL if one already exists. */
a52_fujinet_t *a52_fujinet_create(void);
void a52_fujinet_destroy(a52_fujinet_t *c);

/* Power on (also a power cycle: powers off first). What it serves, as MAME's
 * device_reset does:
 *   image NULL, or the baked CONFIG's bytes : the CONFIG client, mailbox live;
 *   an image with the "FUJI" claim and exactly 32K : that client at power-on,
 *       mailbox live (it is a FujiNet app);
 *   any other image the planner (a52map_plan) accepts : served as if staged,
 *       armed and swapped in (mailbox OFF, no socket): a plain game.
 * `boip` is fujinet-pc's BoIP listener "host:port", or "" for none at all
 * (FujiNet switched off: every transaction fails at once). `sync` runs the
 * mailbox inline. Returns 0, or -1 with the reason in `why`. */
int a52_fujinet_power_on(a52_fujinet_t *c, const uint8_t *image, uint32_t len,
                         const char *boip, bool sync, char *why, int whysz);
void a52_fujinet_power_off(a52_fujinet_t *c);
bool a52_fujinet_powered(const a52_fujinet_t *c);

/* ---- the console's side (emulation thread) ---- */
/* A CPU access at $4000-$BFFF. Read: the byte served by the live view; with
 * no_side_effects the mapper and mailbox are NOT touched (the debugger).
 * Write: a cycle on the edge, same side effects as a read, byte lost. */
uint8_t a52_fujinet_read(a52_fujinet_t *c, uint16_t addr, int no_side_effects);
void    a52_fujinet_write(a52_fujinet_t *c, uint16_t addr, uint8_t value);

/* ANTIC reads cart ROM straight from atari800's MEMORY_mem, not through the
 * CPU's read map. view_serial() moves whenever the live view's pages change
 * (a swap, a bank switch, power on); then copy_window() refreshes the 32K
 * ($4000-$BFFF) the host keeps in MEMORY_mem: the live view as ANTIC sees it,
 * with the mailbox arena page ($B000-$B7FF) as the arena currently holds it
 * (so the stub's opcodes are fetchable). */
unsigned a52_fujinet_view_serial(const a52_fujinet_t *c);
void     a52_fujinet_copy_window(a52_fujinet_t *c, uint8_t dst[0x8000]);

/* Once a frame: bring what the mailbox worker published into the view. */
void a52_fujinet_frame(a52_fujinet_t *c);

/* ---- planning, for the session (no cart needed) ---- */
/* 1 if `image` can run on the cartridge: *claims set when it carries the FUJI
 * claim, *kind the A52MAP_* mapper, *crc the CRC-32 of the image
 * after any .car header. Else 0 and why. */
int a52_fujinet_check_image(const uint8_t *image, uint32_t len, bool *claims,
                            int *kind, uint32_t *crc, char *why, int whysz);
const char *a52_fujinet_kind_name(int kind);      /* "ROM", "2 chips", "Bounty Bob", "Super Cart" */
/* The baked CONFIG client's bytes (fujiconfigrom.h). */
const uint8_t *a52_fujinet_config_image(uint32_t *len);

/* ---- status, for the session and the debugger (any thread) ---- */
typedef struct {
    bool powered;
    bool mailbox;             /* the running view owns the arena page */
    bool link_up;             /* the BoIP socket is connected */
    bool link_off;            /* FujiNet is switched off: nothing is dialled */
    bool claim;               /* the running image carries the FUJI claim */
    bool booted_game;         /* anything other than the baked CONFIG has been served */
    bool armed;               /* the client armed the swap */
    bool staged;              /* a staged image exists */
    bool staged_claim, staged_config;
    int  mapper;              /* A52MAP_* of the live image */
    int  staged_mapper;
    int  nbanks;              /* Super Cart: 32K banks (2 or 4), else 0 */
    int  bank[2];             /* Bounty Bob: the two windows; Super Cart: bank[0] */
    uint32_t live_size, live_crc;
    uint32_t staged_size, staged_crc;
    unsigned swaps;           /* images swapped in since power-on */
    int boot_state, boot_pct, boot_err;
    uint8_t ackseq, status, err, reply_cmd;
    uint16_t rxlen;
    unsigned queue;           /* hotspot reads waiting for the worker */
} a52_cart_status_t;
void a52_fujinet_status(a52_fujinet_t *c, a52_cart_status_t *out);

/* The 2K arena ($B000-$B7FF) as the console would read it, no side effects. */
#define A52_FJ_ARENA_BYTES 0x800
void a52_fujinet_arena(a52_fujinet_t *c, uint8_t out[A52_FJ_ARENA_BYTES]);

/* The live view's byte at a window offset ($0000-$7FFF = CPU $4000-$BFFF),
 * no side effects (the debugger's cart space). Edit: the view's own buffer,
 * never a hotspot. */
uint8_t a52_fujinet_view_byte(a52_fujinet_t *c, uint32_t offset);
void    a52_fujinet_view_poke(a52_fujinet_t *c, uint32_t offset, uint8_t data);

#ifdef __cplusplus
}
#endif

#endif /* A52_FUJINET_CART_H */
