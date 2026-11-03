/*
 * a52debug -- the debugger engine every frontend's debugger window drives:
 * atari800's 6502 stopped and stepped (5200 mode), its memory, ANTIC, GTIA,
 * POKEY (with the 5200's pots and keypad), the memory map and the FujiNet
 * cartridge (mapper, bank, mailbox arena, swap state), breakpoints and
 * watchpoints, the FujiNet tab (the cartridge's BoIP/FujiBus traffic,
 * SLIP-decoded) and atari800's own monitor as a console. No PIA, no SIO.
 *
 * Attaching stops the machine; detaching lets it run (breakpoints stay,
 * disarmed). Calls are safe from any thread.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52DEBUG_H
#define A52DEBUG_H

#include <stdint.h>

#include "a52session.h"

#ifdef __cplusplus
extern "C" {
#endif

a52debug *a52debug_get(a52session *s);

void a52debug_attach(a52debug *d);
void a52debug_detach(a52debug *d);
int  a52debug_is_attached(a52debug *d);
int  a52debug_is_stopped(a52debug *d);
void a52debug_stop(a52debug *d);          /* break before the next instruction */
void a52debug_resume(a52debug *d);        /* F5 */
/* Why it stopped ("Breakpoint 2", "Step", "BRK", "CIM" ...); the PC. */
int  a52debug_stop_reason(a52debug *d, char *dst, int dstsz, int *address);
/* Moves on every stop and resume (frontends refresh on a change). */
unsigned a52debug_generation(a52debug *d);

/* atari800's monitor: one command line; its output into dst. Returns
   A52DBG_CMD_OK, _RESUMED (the command let the machine go: G, R, O,
   CONT) or _NEEDS_INPUT (it is asking for more: the A assembler). */
enum { A52DBG_CMD_OK = 0, A52DBG_CMD_RESUMED = 1, A52DBG_CMD_NEEDS_INPUT = 2 };
int  a52debug_command(a52debug *d, const char *line, char *dst, int dstsz);
/* The monitor commands starting with prefix, space-separated; returns the
   text's length. */
int  a52debug_completions(a52debug *d, const char *prefix, char *dst, int dstsz);

void a52debug_step(a52debug *d);          /* F7: one instruction */
void a52debug_step_over(a52debug *d);     /* F8: over a JSR or branch */
void a52debug_step_out(a52debug *d);      /* Shift+F8: to the RTS/RTI */
void a52debug_frame(a52debug *d);         /* to the next vertical blank */
void a52debug_run_to(a52debug *d, uint16_t addr);
void a52debug_break_on_brk(a52debug *d, int on);
int  a52debug_breaks_on_brk(a52debug *d);

/* ---- the CPU ------------------------------------------------------------- */
typedef struct {
    int pc, sp, a, x, y, p;
    int n, v, d, i, z, c;
    int scanline, cycle;        /* ANTIC's ypos and xpos */
    uint32_t frame;
} a52debug_cpu;
void a52debug_cpu_get(a52debug *d, a52debug_cpu *out);
typedef enum { A52_REG_PC, A52_REG_SP, A52_REG_A, A52_REG_X, A52_REG_Y, A52_REG_P,
               A52_FLAG_N, A52_FLAG_V, A52_FLAG_D, A52_FLAG_I, A52_FLAG_Z,
               A52_FLAG_C } a52debug_reg;
void a52debug_cpu_set(a52debug *d, int reg, int value);

/* The last instructions executed, oldest first. */
typedef struct {
    uint16_t pc;
    int scanline, cycle;
    char disasm[64];
} a52debug_history;
int  a52debug_history_get(a52debug *d, a52debug_history *out, int max);

/* ---- memory and code ------------------------------------------------------ */
/* As the CPU sees it (banking applied; NO side effects: the cartridge's
   mailbox and mapper hotspots are not triggered). */
int  a52debug_read(a52debug *d, uint16_t addr, uint8_t *dst, int n);
void a52debug_write(a52debug *d, uint16_t addr, uint8_t value);

typedef struct {
    uint16_t address;
    int is_pc;
    int has_breakpoint;         /* 1 enabled, 2 disabled */
    char bytes[16];
    char label[48];
    char disasm[64];
    char comment[64];
} a52debug_line;
int  a52debug_disassemble(a52debug *d, uint16_t addr, a52debug_line *out, int max,
                           int *pc_line);
/* An address rows instructions before (rows < 0) or after addr. */
int  a52debug_row_address(a52debug *d, uint16_t addr, int rows);
int  a52debug_label_address(a52debug *d, const char *label);
int  a52debug_address_label(a52debug *d, uint16_t addr, char *dst, int dstsz);
/* Labels from a ca65/ld65 (-Ln) or VICE .lbl, MADS .lab or atari800 label
   file. 5200 BIOS and hardware labels are built in. */
int  a52debug_load_symbols(a52debug *d, const char *path, char *msg, int msgsz);

/* ---- ANTIC --------------------------------------------------------------- */
typedef struct {
    uint8_t dmactl, chactl, hscrol, vscrol, pmbase, chbase, nmien, nmist;
    uint16_t dlist;             /* DLISTL/H */
    int scanline, cycle, vcount;
    int playfield_width;        /* 0 none, 1 narrow, 2 normal, 3 wide */
    int dl_dma, pm_dma, missile_dma, pm_single_line;
} a52debug_antic;
void a52debug_antic_get(a52debug *d, a52debug_antic *out);
/* The display list, one instruction a line ("$9C20  LMS 4  $9C40 ..."). */
int  a52debug_dlist_text(a52debug *d, char *dst, int dstsz);

/* ---- GTIA ---------------------------------------------------------------- */
typedef struct {
    uint8_t hposp[4], hposm[4], sizep[4], sizem, grafp[4], grafm;
    uint8_t colpm[4], colpf[4], colbk, prior, vdelay, gractl, consol;
    uint8_t mpf[4], ppf[4], mpl[4], ppl[4];   /* collisions */
    uint8_t trig[4];
} a52debug_gtia;
void a52debug_gtia_get(a52debug *d, a52debug_gtia *out);
uint32_t a52debug_color(a52debug *d, uint8_t index);     /* XRGB */
#define A52DEBUG_PALETTE_WIDTH  256
#define A52DEBUG_PALETTE_HEIGHT 64
/* The 256 colours, 16 hues down and 16 lumas across. */
int  a52debug_palette_image(a52debug *d, uint32_t *dst);

/* ---- POKEY ---------------------------------------------------------------- */
typedef struct {
    uint8_t audf[4], audc[4], audctl;
    uint8_t kbcode, skstat, skctl, irqen, irqst, random;
    uint8_t pot[8];
    int second;                 /* a stereo POKEY's second chip */
} a52debug_pokey;
void a52debug_pokey_get(a52debug *d, int chip, a52debug_pokey *out);

/* ---- the memory map and the FujiNet cartridge ----------------------------- */
typedef struct {
    char bios[48];              /* "Built-in Altirra 5200 OS", "Atari 5200 BIOS (4-port)" */
    int tv_pal;
    /* the cartridge */
    int cart_powered;
    int mapper;                 /* A52MAP_*: 0 rom, 1 2chips, 2 bbsb, 3 supercart */
    const char *mapper_name;    /* "ROM", "2 chips", "Bounty Bob", "Super Cart" */
    int nbanks;                 /* Super Cart 32K banks, else 0 */
    int bank[2];                /* Bounty Bob: the two windows; Super Cart: bank[0] */
    int mailbox;                /* the running image owns the arena $B000-$B7FF */
    int link_up, link_off;      /* the BoIP socket */
    int claim;                  /* the running image carries the "FUJI" claim */
    int booted_game;            /* anything but the baked CONFIG has been served */
    int armed, staged;          /* swap armed / an image is staged */
    int staged_claim, staged_config;
    int staged_mapper;
    uint32_t live_size, live_crc, staged_size, staged_crc;
    int swaps;                  /* images swapped in since power-on */
    int boot_state, boot_pct, boot_err;
    uint8_t ackseq, status, last_error, reply_cmd;
    uint16_t rxlen;
    uint32_t queue_depth;       /* hotspot reads waiting for the cartridge's worker */
    /* the arena as the console reads it, with no side effects:
       $B000-$B3FF reply window, $B400-$B4FF status page, $B500-$B7FF the
       REGSEL / REGDATA / TX hotspot pages */
    uint8_t arena[0x800];
    char cart_name[96];         /* "FujiNet CONFIG", "FujiNet app", "Super Cart 128 KB" */
} a52debug_map;
void a52debug_map_get(a52debug *d, a52debug_map *out);
/* One line: "FujiNet cartridge: CONFIG, link up". */
int  a52debug_cart_info(a52debug *d, char *dst, int dstsz);

/* ---- breakpoints and watchpoints ------------------------------------------ */
typedef enum { A52DEBUG_BP_EXEC = 1, A52DEBUG_BP_READ = 2, A52DEBUG_BP_WRITE = 4 } a52debug_bp_type;
typedef struct {
    int id;                     /* breakpoints 1..; watchpoints 1001.. */
    int type;                   /* a52debug_bp_type bits */
    uint16_t start, end;
    int enabled;
    char condition[96];         /* "A=$10 && X<4" ("" none) */
} a52debug_breakpoint;
int  a52debug_breakpoint_toggle(a52debug *d, uint16_t addr);
int  a52debug_breakpoint_check(a52debug *d, uint16_t addr);
/* The new id, or -1 with the reason in msg (a bad condition, or atari800's
   table of 20 conditions full). */
int  a52debug_breakpoint_add(a52debug *d, int type, uint16_t start, uint16_t end,
                              const char *condition, char *msg, int msgsz);
void a52debug_breakpoint_remove(a52debug *d, int id);
void a52debug_breakpoint_enable(a52debug *d, int id, int enabled);
int  a52debug_breakpoint_list(a52debug *d, a52debug_breakpoint *out, int max);
void a52debug_breakpoint_clear(a52debug *d);
/* Entries of atari800's condition table in use, of max. */
int  a52debug_breakpoint_budget(a52debug *d, int *max);

/* ---- FujiNet: the cartridge's BoIP socket, as FujiBus packets (always on) -- */
typedef struct {
    uint64_t serial;
    int      to_host;       /* 1: FujiNet -> 5200, 0: 5200 -> FujiNet */
    int      device, command, length;   /* from the FujiBus header, -1 if short */
    int      nbytes;        /* the decoded packet's size */
    uint8_t  data[64];      /* its first bytes */
    char     text[96];      /* "5200 -> FujiNet 70 F4 READ HOST SLOTS (6 bytes)" */
    uint32_t repeat;        /* the same exchange back to back, shown "xN" */
} a52_fuji_packet;
/* Recorded from session start, attached or not. The newest `max`, oldest
   first; returns the count. */
int  a52debug_fujinet_trace(a52debug *d, a52_fuji_packet *out, int max);
void a52debug_fujinet_trace_clear(a52debug *d);
/* The cartridge's link and mailbox, as text lines. Returns the length. */
int  a52debug_fujinet_state(a52debug *d, char *dst, int dstsz);

/* "dis" (the disassembly window), "ram" (64K as the CPU sees it, no side
   effects), "cart" (the running cartridge's 32K window, $4000-$BFFF),
   "arena" (the mailbox arena, 2K), "screen" (the frame as a PPM). */
int  a52debug_save(a52debug *d, const char *kind, const char *path, char *msg, int msgsz);

#ifdef __cplusplus
}
#endif

#endif
