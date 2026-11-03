/*
 * The session's private state, shared by the core's C and C++ files.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52_SESSION_INTERNAL_H
#define A52_SESSION_INTERNAL_H

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>

#include "a52session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A52_PATH_MAX 1024

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

/* BIOS slots (roms.c): the 5200's two real BIOS revisions. */
enum { ROM_5200_4PORT = 0, ROM_5200_2PORT, ROM_COUNT };

struct a52session {
    char config_dir[A52_PATH_MAX];
    char data_dir[A52_PATH_MAX];
    char media_dir[A52_PATH_MAX];          /* where games are kept */
    char roms_dir[A52_PATH_MAX];           /* imported BIOS images */
    char settings_file[A52_PATH_MAX];
    setting_kv *settings;
    pthread_mutex_t settings_mtx;
    int settings_dirty;
    char last_error[256];

    char fujinet_root[A52_PATH_MAX];       /* <data>/fujinet */
    char fujinet_config[A52_PATH_MAX];     /* .../fnconfig.ini */
    char fujinet_sd[A52_PATH_MAX];         /* .../SD */
    char fujinet_data[A52_PATH_MAX];       /* .../data */
    char fujinet_lib[A52_PATH_MAX];        /* resolved libfujinet, "" until then */
    char fujinet_runtime_src[A52_PATH_MAX];
    char webui_url[64];
    int  fujinet_running;
    int  fujinet_wanted;                    /* FujiNet enabled for this run */
    int  fujinet_external;                  /* ... and it is not ours (wanted stays 1) */
    char boip_endpoint[80];                 /* "host:port" the cartridge dials */
    char boip_addr[80];                     /* the external address as typed */
    char cart_path[A52_PATH_MAX];           /* the opened game ("" = CONFIG) */
    int  fujinet_disabled;                  /* the caller said no FujiNet ("" lib) */

    pthread_mutex_t sysact_mtx;
    unsigned sysact_pending;

    a52session_start_opts opts;
    int running;

    void *impl;             /* session.cpp's SessionImpl */
    void *audio;            /* audio_sdl.c, NULL until started */
    void *gamepad;          /* gamepad_sdl.c, NULL until started */
    void *debugger;         /* a52debug, lazily created */

    pthread_mutex_t pad_event_mtx;
    char pad_event[128];
};

void settings_init(struct a52session *s);
void settings_free_all(struct a52session *s);

int  paths_init(struct a52session *s, const char *config_dir, const char *data_dir);
int  paths_provision_fujinet(struct a52session *s);
int  paths_mkdir_p(const char *path);

void session_set_error(struct a52session *s, const char *fmt, ...);
void session_roms_changed(struct a52session *s);

/* prefs.c */
typedef void (*prefs_emit_fn)(void *u, const char *key, const char *val);
/* atari800 config options (CFG_ApplyOption): the BIOS, TV system, sound... */
void prefs_emit_options(struct a52session *s, prefs_emit_fn emit, void *u);
void prefs_emit_live(struct a52session *s, prefs_emit_fn emit, void *u);
/* The BIOS the "bios" preference resolves to: the ROM_* slot of an imported
 * image, or -1 for the built-in Altirra OS. */
int  prefs_bios_slot(struct a52session *s);

/* roms.c */
const char *roms_config_key(int slot);               /* atari800's, "5200_ROM"... */
const char *roms_path(struct a52session *s, int slot); /* NULL if not imported */
uint32_t roms_crc32(const uint8_t *p, size_t n);
/* 1 if path (a file, a zip, a folder) holds at least one recognised BIOS. */
int  roms_identify_file(const char *path);
void roms_provision_embedded(struct a52session *s);

/* fujinet_runtime.c (the in-process FujiNet: FujiNet listens, the cartridge dials) */
int  session_fujinet_available(struct a52session *s);
int  session_fujinet_start(struct a52session *s);
void session_fujinet_stop(struct a52session *s);
int  session_fujinet_wait_for_boip(struct a52session *s, int timeout_ms);
/* speech from FujiNet, mixed into the audio callback */
void session_fujinet_mix(struct a52session *s, float *stereo, int frames, float gain);

/* audio_sdl.c, gamepad_sdl.c */
int  audio_start(struct a52session *s);
void audio_stop(struct a52session *s);
int  gamepad_start(struct a52session *s);
void gamepad_stop(struct a52session *s);
void session_gamepad_apply(struct a52session *s, int port, int act, int down);
void session_gamepad_event(struct a52session *s, const char *text);
int  session_active_ports(struct a52session *s);
/* the analog sticks, from the gamepads: x,y -128..127 */
void session_gamepad_stick(struct a52session *s, int port, int x, int y);

#ifdef __cplusplus
}
#endif

#endif
