/* fujinet_cart.c -- the FujiNet Atari 5200 cartridge. See fujinet_cart.h.
 *
 * Transposed from the firmware's MAME device (fujinet-firmware
 * pico/atari-5200/emu/a5200_fujinet.cpp, BSD-3-Clause, copyright-holders
 * Thomas Cherryhomes) onto the staged cartridge sources, with Arcadia's
 * reconnecting link. The names follow the MAME device.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujinet_cart.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* the cartridge firmware's own sources (core/cart/fuji-generated) */
#include "fuji_mailbox.h"
#include "fujibus.h"
#include "fujimail.h"
#include "fujitcp.h"
#include "a52map.h"
#include "fuji_load.h"
#include "fuji_cfg.h"
#include "fuji_crc.h"
#include "fujiconfigrom.h"      /* _configrom, FUJI_CONFIGROM_SIZE */

#include "fujitcp_host.h"

#define QUEUE_SIZE 1024         /* console hotspot reads waiting for the worker */
#define MAX_POKES  4096
#define EV_SWAPPED 0xFFFFu      /* the event queue's marker for a swap */

struct a52_fujinet {
    uint8_t *buf[2];            /* A52MAP_VIEW_MAX each */
    uint8_t arena[FN_ARENA_SIZE];
    fuji_load_t load;
    fuji_cfg_t cfg;
    a52_view_t *view;           /* the view being served (emulation thread) */
    bool powered;
    atomic_uint serial;         /* view_serial */
    bool mailbox_mode;
    atomic_bool mailbox_live;
    bool served_other;

    uint8_t boot[A52MAP_WINDOW];
    uint32_t boot_len;
    uint32_t live_size, live_crc;
    uint32_t staged_size, staged_crc;

    /* the push receiver */
    uint8_t *push;
    uint32_t push_len;

    pthread_mutex_t loadlock;

    bool async, worker_running;
    pthread_t worker;
    pthread_mutex_t qlock;
    pthread_cond_t qcv;
    uint16_t hq[QUEUE_SIZE];
    unsigned qhead, qtail;
    atomic_bool stop;
    pthread_mutex_t pklock;
    uint16_t pk_addr[MAX_POKES];
    uint8_t pk_data[MAX_POKES];
    unsigned npokes;
    atomic_bool poked;

    char boip[96];
    bool have_boip, link_off, debug;
    unsigned pace_ms;
};

/* fujimail's port is C function pointers with no context argument; one slot,
 * one cart, as on the hardware. */
static a52_fujinet_t *s_cart = NULL;

/* set on the worker thread: its pokes queue for the emulation thread */
static _Thread_local bool t_on_worker = false;

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*-------------------------------------------------
    C port callbacks
-------------------------------------------------*/

static void poke(a52_fujinet_t *c, unsigned offset, uint8_t value)
{
    if (offset >= FN_R_PAINT_END)
        return;
    if (!t_on_worker) {
        c->arena[offset] = value;
        return;
    }
    pthread_mutex_lock(&c->pklock);
    if (c->npokes < MAX_POKES) {
        c->pk_addr[c->npokes] = (uint16_t)offset;
        c->pk_data[c->npokes] = value;
        c->npokes++;
    }
    atomic_store_explicit(&c->poked, true, memory_order_release);
    pthread_mutex_unlock(&c->pklock);
}

static void c_poke(unsigned offset, uint8_t value) { poke(s_cart, offset, value); }
static bool c_link_up(void) { return fujitcp_active(); }

/* Dial again if the link has dropped. */
static fb_status_t c_transact(uint8_t device, uint8_t command,
                              const fb_param_t *params, unsigned nparams,
                              const uint8_t *payload, uint16_t payload_len,
                              uint32_t timeout_ms, fb_reply_t *reply)
{
    a52_fujinet_t *c = s_cart;
    fb_status_t st;

    if (!c->link_off && !fujitcp_active())
        fujitcp_init(c->have_boip ? c->boip : NULL);
    st = fujitcp_transact(device, command, params, nparams, payload, payload_len,
                          timeout_ms, reply);
    if (st == FB_ENOLINK)
        fujitcp_close();        /* the next transaction reconnects */
    return st;
}

static void c_send_bare(uint8_t device, uint8_t command, const uint8_t *payload, uint16_t len)
{
    a52_fujinet_t *c = s_cart;

    if (c->pace_ms && device == FUJI_DEVICEID_DBC && command == CMD_FUJI_ACK)
        sleep_ms(c->pace_ms);
    fujitcp_send_bare(device, command, payload, len);
}

/* Try to connect for up to `ms`: the listener can be a moment behind the
 * console on a first start. */
static void c_wait_link(uint32_t ms)
{
    a52_fujinet_t *c = s_cart;
    const uint64_t deadline = now_ms() + ms;

    while (!c->link_off && !fujitcp_active() && !atomic_load(&c->stop)) {
        if (fujitcp_init(c->have_boip ? c->boip : NULL) == 0)
            break;
        if (now_ms() >= deadline)
            break;
        sleep_ms(100);
    }
    poke(c, FN_R_STATUS, fujitcp_active() ? FN_R_STATUS_LINK : 0);
}

static void c_on_txn(const fujimail_txn_t *t)
{
    char txt[40];
    unsigned k, m = 0;

    for (k = 0; k < t->rxlen && m < sizeof txt - 1; k++) {
        uint8_t ch = t->rx[k];

        if (ch == 0)
            break;
        txt[m++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    txt[m] = '\0';
    fprintf(stderr,
            "fujinet: dev=%02X cmd=%02X nparam=%u txlen=%u seq=%u"
            " -> err=%d reply=%02X rxlen=%u%s%s%s\n",
            t->device, t->command, t->nparam, t->txlen, t->seq,
            t->status, t->reply_cmd, t->rxlen,
            m ? " \"" : "", txt, m ? "\"" : "");
}

static void c_on_dbc(fujimail_dbc_ev_t ev, int stream, uint32_t expect, unsigned got, bool aborted)
{
    if (ev == FUJIMAIL_DBC_OPEN)
        fprintf(stderr, "fujinet: DBC open stream=%d size=%u\n", stream, expect);
    else
        fprintf(stderr, "fujinet: DBC close stream=%d got=%u%s\n",
                stream, got, aborted ? " ABORTED" : "");
}

static uint8_t c_stream_open(int stream, uint32_t size);
static void c_stream_write(int stream, const uint8_t *chunk, unsigned len);
static uint8_t c_stream_close(int stream, uint32_t got, bool aborted);
static void c_arm_swap(void);
static void c_own_reg(uint8_t reg, uint8_t value);

static const fujimail_port_t debug_port = {
    .poke = c_poke, .link_up = c_link_up, .transact = c_transact,
    .send_bare = c_send_bare, .stream_open = c_stream_open,
    .stream_write = c_stream_write, .stream_close = c_stream_close,
    .arm_swap = c_arm_swap, .wait_link_ms = c_wait_link,
    .on_txn = c_on_txn, .on_dbc = c_on_dbc, .own_reg = c_own_reg,
};

static const fujimail_port_t quiet_port = {
    .poke = c_poke, .link_up = c_link_up, .transact = c_transact,
    .send_bare = c_send_bare, .stream_open = c_stream_open,
    .stream_write = c_stream_write, .stream_close = c_stream_close,
    .arm_swap = c_arm_swap, .wait_link_ms = c_wait_link,
    .own_reg = c_own_reg,
};

/*-------------------------------------------------
    staging
-------------------------------------------------*/

static void drain_pokes(a52_fujinet_t *c)
{
    uint16_t addr[MAX_POKES];
    uint8_t data[MAX_POKES];
    unsigned n;

    if (!atomic_load_explicit(&c->poked, memory_order_acquire))
        return;
    pthread_mutex_lock(&c->pklock);
    n = c->npokes;
    memcpy(addr, c->pk_addr, n * sizeof addr[0]);
    memcpy(data, c->pk_data, n);
    c->npokes = 0;
    atomic_store_explicit(&c->poked, false, memory_order_relaxed);
    pthread_mutex_unlock(&c->pklock);
    for (unsigned i = 0; i < n; i++)
        c->arena[addr[i]] = data[i];
}

static void publish_staged(a52_fujinet_t *c)
{
    a52map_plan_t p;
    bool staged, staged_config, armed;
    uint8_t v = 0;

    pthread_mutex_lock(&c->loadlock);
    p = c->load.plan[c->load.live ^ 1u];
    staged = c->load.staged;
    staged_config = c->load.staged_config;
    armed = c->load.armed;
    pthread_mutex_unlock(&c->loadlock);

    if (staged) {
        v = FN_STAGED_READY;
        if (p.claim)
            v |= FN_STAGED_CLAIM;
        if (staged_config)
            v |= FN_STAGED_CONFIG;
    }
    poke(c, FN_R_STAGED_KIND, p.kind);
    for (unsigned i = 0; i < 4; i++)
        poke(c, FN_R_STAGED_CRC + i, (uint8_t)(p.crc >> (8 * i)));
    poke(c, FN_R_STAGED, v);
    poke(c, FN_R_ARMED, armed ? 1 : 0);
}

static uint8_t c_stream_open(int stream, uint32_t size)
{
    a52_fujinet_t *c = s_cart;
    uint8_t err;

    if (stream != FN_STREAM_ROM) {
        fuji_cfg_open(&c->cfg);
        return 0;
    }
    err = a52map_gate(size);
    if (err)
        return err;
    pthread_mutex_lock(&c->loadlock);
    c->push = fuji_load_target(&c->load);
    pthread_mutex_unlock(&c->loadlock);
    c->push_len = 0;
    publish_staged(c);
    return 0;
}

static void c_stream_write(int stream, const uint8_t *chunk, unsigned len)
{
    a52_fujinet_t *c = s_cart;

    if (stream != FN_STREAM_ROM) {
        fuji_cfg_write(&c->cfg, chunk, len);
        return;
    }
    if (c->push && c->push_len + len <= A52MAP_IMAGE_MAX + A52MAP_CAR_HEADER) {
        memcpy(c->push + c->push_len, chunk, len);
        c->push_len += len;
    }
}

static uint8_t c_stream_close(int stream, uint32_t got, bool aborted)
{
    a52_fujinet_t *c = s_cart;
    const char *cfg;
    char mapper[FUJI_CFG_MAPPER_MAX + 1];
    uint8_t err;
    a52map_plan_t p;

    if (stream != FN_STREAM_ROM) {
        /* the .cfg sibling: its mapper= applies to the next image */
        fuji_cfg_close(&c->cfg, aborted);
        return 0;
    }
    cfg = fuji_cfg_mapper(&c->cfg);
    snprintf(mapper, sizeof mapper, "%s", cfg ? cfg : "");
    fuji_cfg_clear(&c->cfg);
    if (aborted || !c->push || c->push_len == 0) {
        c->push = NULL;
        return 0;
    }
    if (got > c->push_len) {            /* an OPEN with no size let it outgrow the buffer */
        c->push = NULL;
        return FN_BOOT_ERR_TOOBIG;
    }
    if (mapper[0])
        fprintf(stderr, "fujinet: .cfg mapper=%s\n", mapper);
    pthread_mutex_lock(&c->loadlock);
    err = fuji_load_commit(&c->load, c->push_len, mapper[0] ? mapper : NULL);
    p = c->load.plan[c->load.live ^ 1u];
    if (!err) {
        c->staged_size = p.size;
        c->staged_crc = p.crc;
    }
    pthread_mutex_unlock(&c->loadlock);
    c->push = NULL;
    publish_staged(c);
    if (!err)
        fprintf(stderr, "fujinet: staged %u bytes as %s (src %u)%s\n", p.size,
                a52map_kind_name(p.kind), p.src, p.claim ? ", claimed" : "");
    return err;
}

static void c_arm_swap(void)
{
    a52_fujinet_t *c = s_cart;

    pthread_mutex_lock(&c->loadlock);
    fuji_load_arm(&c->load);
    pthread_mutex_unlock(&c->loadlock);
    publish_staged(c);
}

/* The cart's own register, handed over by fujimail (an app going back to CONFIG). */
static void c_own_reg(uint8_t reg, uint8_t value)
{
    a52_fujinet_t *c = s_cart;

    if (reg == FN_REG_CONFIG && value == FN_CONFIG_MAGIC) {
        pthread_mutex_lock(&c->loadlock);
        /* always the baked CONFIG, as on the cartridge (MAME's device
         * restores m_boot, which is the opened client when one was given) */
        fuji_load_stage_config(&c->load, _configrom, FUJI_CONFIGROM_SIZE);
        fuji_load_arm(&c->load);
        c->staged_size = FUJI_CONFIGROM_SIZE;
        c->staged_crc = fuji_crc32(0, _configrom, FUJI_CONFIGROM_SIZE);
        pthread_mutex_unlock(&c->loadlock);
        publish_staged(c);
    }
}

/*-------------------------------------------------
    the worker (core0)
-------------------------------------------------*/

/* A swap the emulation thread made: repaint and republish from the worker. */
static void service(a52_fujinet_t *c, uint16_t a)
{
    uint8_t swaps, kind;
    bool mailbox;

    if (a != EV_SWAPPED) {
        fujimail_read_hotspot(a);
        return;
    }
    pthread_mutex_lock(&c->loadlock);
    swaps = c->load.swaps;
    kind = c->load.plan[c->load.live].kind;
    mailbox = c->load.view[c->load.live].mailbox;
    pthread_mutex_unlock(&c->loadlock);
    if (mailbox)
        fujimail_paint();
    publish_staged(c);
    poke(c, FN_R_SWAPS, swaps);
    poke(c, FN_R_MODE, mailbox ? FN_MODE_APP : FN_MODE_GAME);
    poke(c, FN_R_MAPPER, kind);
}

static void hotspot(a52_fujinet_t *c, uint16_t a)
{
    if (!c->async) {
        service(c, a);
        return;
    }
    pthread_mutex_lock(&c->qlock);
    {
        unsigned next = (c->qhead + 1) % QUEUE_SIZE;

        if (next != c->qtail) {
            c->hq[c->qhead] = a;
            c->qhead = next;
        }
    }
    pthread_cond_signal(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
}

static void *worker_main(void *arg)
{
    a52_fujinet_t *c = arg;

    t_on_worker = true;
    for (;;) {
        uint16_t a;

        pthread_mutex_lock(&c->qlock);
        while (c->qhead == c->qtail && !atomic_load(&c->stop))
            pthread_cond_wait(&c->qcv, &c->qlock);
        if (atomic_load(&c->stop)) {
            pthread_mutex_unlock(&c->qlock);
            break;
        }
        a = c->hq[c->qtail];
        c->qtail = (c->qtail + 1) % QUEUE_SIZE;
        pthread_mutex_unlock(&c->qlock);
        service(c, a);
    }
    return NULL;
}

static void worker_start(a52_fujinet_t *c)
{
    atomic_store(&c->stop, false);
    c->qhead = c->qtail = 0;
    c->worker_running = false;
    if (!c->async)
        return;
    if (pthread_create(&c->worker, NULL, worker_main, c) == 0) {
        c->worker_running = true;
    } else {
        fprintf(stderr, "fujinet: no mailbox worker; servicing inline\n");
        c->async = false;
    }
}

/* A transaction in flight is cut short, so a power-off or exit never waits out
 * its timeout. */
static void worker_stop(a52_fujinet_t *c)
{
    if (!c->worker_running)
        return;
    pthread_mutex_lock(&c->qlock);
    atomic_store(&c->stop, true);
    pthread_cond_broadcast(&c->qcv);
    pthread_mutex_unlock(&c->qlock);
    fujitcp_abort();
    pthread_join(c->worker, NULL);
    c->worker_running = false;
    c->qhead = c->qtail = 0;
    pthread_mutex_lock(&c->pklock);
    c->npokes = 0;
    atomic_store(&c->poked, false);
    pthread_mutex_unlock(&c->pklock);
}

/*-------------------------------------------------
    create / power
-------------------------------------------------*/

a52_fujinet_t *a52_fujinet_create(void)
{
    a52_fujinet_t *c;

    if (s_cart)
        return NULL;
    c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->buf[0] = malloc(A52MAP_VIEW_MAX);
    c->buf[1] = malloc(A52MAP_VIEW_MAX);
    if (!c->buf[0] || !c->buf[1]) {
        free(c->buf[0]);
        free(c->buf[1]);
        free(c);
        return NULL;
    }
    memset(c->buf[0], 0xff, A52MAP_VIEW_MAX);
    memset(c->buf[1], 0xff, A52MAP_VIEW_MAX);
    atomic_init(&c->serial, 0);
    atomic_init(&c->mailbox_live, false);
    atomic_init(&c->stop, false);
    atomic_init(&c->poked, false);
    pthread_mutex_init(&c->loadlock, NULL);
    pthread_mutex_init(&c->qlock, NULL);
    pthread_cond_init(&c->qcv, NULL);
    pthread_mutex_init(&c->pklock, NULL);
    s_cart = c;
    return c;
}

void a52_fujinet_destroy(a52_fujinet_t *c)
{
    if (!c)
        return;
    a52_fujinet_power_off(c);
    if (s_cart == c)
        s_cart = NULL;
    free(c->buf[0]);
    free(c->buf[1]);
    pthread_mutex_destroy(&c->loadlock);
    pthread_mutex_destroy(&c->qlock);
    pthread_cond_destroy(&c->qcv);
    pthread_mutex_destroy(&c->pklock);
    free(c);
}

static void set_why(char *why, int whysz, const char *text)
{
    if (why && whysz > 0)
        snprintf(why, (size_t)whysz, "%s", text);
}

static const char *plan_error(int err)
{
    switch (err) {
    case A52MAP_EEMPTY:       return "the cartridge image is empty";
    case A52MAP_ETOOBIG:      return "the image is larger than the cartridge can hold (128 KB)";
    case A52MAP_EUNSUPPORTED: return "the image is not a cartridge this FujiNet cart can map";
    default:                  return "the image is not a cartridge this FujiNet cart can map";
    }
}

const char *a52_fujinet_kind_name(int kind)
{
    switch (kind) {
    case A52MAP_ROM:       return "ROM";
    case A52MAP_2CHIPS:    return "2 chips";
    case A52MAP_BBSB:      return "Bounty Bob";
    case A52MAP_SUPERCART: return "Super Cart";
    default:               return "unknown";
    }
}

const uint8_t *a52_fujinet_config_image(uint32_t *len)
{
    if (len)
        *len = FUJI_CONFIGROM_SIZE;
    return _configrom;
}

int a52_fujinet_check_image(const uint8_t *image, uint32_t len, bool *claims,
                            int *kind, uint32_t *crc, char *why, int whysz)
{
    a52map_plan_t plan;
    int err;

    if (claims)
        *claims = false;
    if (kind)
        *kind = -1;
    if (crc)
        *crc = 0;
    if (!image || !len) {
        set_why(why, whysz, "the cartridge image is empty");
        return 0;
    }
    err = a52map_plan(image, len, NULL, &plan);
    if (err != A52MAP_OK) {
        set_why(why, whysz, plan_error(err));
        return 0;
    }
    if (claims)
        *claims = plan.claim;
    if (kind)
        *kind = plan.kind;
    if (crc)
        *crc = plan.crc;
    return 1;
}

static void bump(a52_fujinet_t *c)
{
    atomic_fetch_add(&c->serial, 1);
}

/* A swap the emulation thread has made: tell the worker to repaint. */
static void swapped(a52_fujinet_t *c)
{
    a52map_plan_t p;

    pthread_mutex_lock(&c->loadlock);
    p = c->load.plan[c->load.live];
    c->served_other = true;
    c->live_size = p.size;
    c->live_crc = p.crc;
    pthread_mutex_unlock(&c->loadlock);
    atomic_store(&c->mailbox_live, c->view->mailbox);
    fprintf(stderr, "fujinet: swapped in %s (%u bytes, crc %08X); mailbox %s\n",
            a52map_kind_name(p.kind), p.size, p.crc, c->view->mailbox ? "kept" : "off");
    bump(c);
    if (c->view->mailbox)
        hotspot(c, EV_SWAPPED);
}

int a52_fujinet_power_on(a52_fujinet_t *c, const uint8_t *image, uint32_t len,
                         const char *boip, bool sync, char *why, int whysz)
{
    a52map_plan_t plan;
    const uint8_t *direct = NULL;
    uint32_t direct_len = 0;
    const char *env;
    int err;

    if (!c)
        return -1;
    a52_fujinet_power_off(c);

    if (image && len) {
        err = a52map_plan(image, len, NULL, &plan);
        if (err != A52MAP_OK) {
            set_why(why, whysz, plan_error(err));
            return -1;
        }
    }

    c->debug = getenv("FUJINET_DEBUG") != NULL;
    env = getenv("FUJINET_PACE_MS");
    c->pace_ms = env ? (unsigned)atoi(env) : 0;

    memcpy(c->boot, _configrom, FUJI_CONFIGROM_SIZE);
    c->boot_len = FUJI_CONFIGROM_SIZE;
    if (image && len) {
        if (plan.claim && len == A52MAP_WINDOW) {
            memcpy(c->boot, image, len);
            c->boot_len = len;
        } else {
            direct = image;
            direct_len = len;
        }
    }

    memset(c->arena, 0, FN_H_REGSEL);
    memset(c->arena + FN_H_REGSEL, 0xFF, FN_ARENA_SIZE - FN_H_REGSEL);
    c->arena[FN_H_REGSEL + FN_HOT_STUB] = 0x6C;
    c->arena[FN_H_REGSEL + FN_HOT_STUB + 1] = 0xFC;
    c->arena[FN_H_REGSEL + FN_HOT_STUB + 2] = 0xFF;
    c->arena[FN_H_REGSEL + FN_HOT_SWAP] = 0xFF;
    fuji_cfg_clear(&c->cfg);
    c->push = NULL;
    c->push_len = 0;
    c->npokes = 0;
    atomic_store(&c->poked, false);
    c->staged_size = c->staged_crc = 0;
    c->served_other = false;

    pthread_mutex_lock(&c->loadlock);
    fuji_load_init(&c->load, c->buf[0], c->buf[1], c->arena, c->boot, c->boot_len);
    pthread_mutex_unlock(&c->loadlock);

    if (direct) {
        /* served at power-on as if the cart had planned and swapped it in */
        uint8_t *t = fuji_load_target(&c->load);
        uint32_t n = direct_len;

        if (n > A52MAP_IMAGE_MAX + A52MAP_CAR_HEADER)
            n = A52MAP_IMAGE_MAX + A52MAP_CAR_HEADER;
        memcpy(t, direct, n);
        if (fuji_load_commit(&c->load, n, NULL) != 0) {
            fprintf(stderr, "fujinet: image refused; serving CONFIG\n");
        } else {
            fuji_load_arm(&c->load);
            (void)fuji_load_swap(&c->load);
            c->served_other = true;
            c->live_size = c->load.plan[c->load.live].size;
            c->live_crc = c->load.plan[c->load.live].crc;
        }
    }
    if (!c->served_other) {
        c->live_size = c->boot_len;
        c->live_crc = fuji_crc32(0, c->boot, c->boot_len);
    }
    c->view = fuji_load_live(&c->load);
    c->mailbox_mode = c->view->mailbox;
    atomic_store(&c->mailbox_live, c->mailbox_mode);
    if (image && len && !direct)
        c->served_other = true;         /* a FUJI app opened in place of CONFIG */

    /* the link: "" = FujiNet switched off */
    c->have_boip = boip && boip[0];
    c->link_off = !c->have_boip;
    snprintf(c->boip, sizeof c->boip, "%s", c->have_boip ? boip : "");
    c->async = !sync;
    env = getenv("FUJINET_ASYNC");
    if (env)
        c->async = atoi(env) != 0;
    c->async = c->async && c->mailbox_mode;
    fujitcp_close();                    /* fujinet-pc's BoIP port takes one client */
    if (c->mailbox_mode) {
        fujimail_init(c->debug ? &debug_port : &quiet_port);
        if (!c->link_off)
            fujitcp_init(c->boip);      /* a miss is retried by the transaction */
        fujimail_paint();
    }
    c->powered = true;
    bump(c);
    if (c->mailbox_mode)
        worker_start(c);
    return 0;
}

void a52_fujinet_power_off(a52_fujinet_t *c)
{
    if (!c || !c->powered)
        return;
    worker_stop(c);
    fujitcp_close();
    c->powered = false;
}

bool a52_fujinet_powered(const a52_fujinet_t *c)
{
    return c && c->powered;
}

/*-------------------------------------------------
    the console's side
-------------------------------------------------*/

/* One committed access, after the byte has been served. */
static void access_cycle(a52_fujinet_t *c, uint32_t off)
{
    a52_view_t *v = c->view;
    const uint8_t *before[A52MAP_PAGES];
    int ev;

    if (!(v->hot & (1u << (off >> 11))))
        return;
    memcpy(before, v->page, sizeof before);
    ev = a52_commit(v, off);
    switch (ev) {
    case A52_EV_MAILBOX:
        hotspot(c, (uint16_t)(off - FN_ARENA_OFF));
        break;
    case A52_EV_SWAP: {
        a52_view_t *nv;

        pthread_mutex_lock(&c->loadlock);
        nv = fuji_load_swap(&c->load);
        pthread_mutex_unlock(&c->loadlock);
        if (nv != v) {
            c->view = nv;
            swapped(c);
        }
        break;
    }
    default:
        if (memcmp(before, v->page, sizeof before) != 0)
            bump(c);
        break;
    }
}

uint8_t a52_fujinet_read(a52_fujinet_t *c, uint16_t addr, int no_side_effects)
{
    uint32_t off;
    uint8_t data;

    if (!c || !c->powered || addr < FN_WINDOW_BASE || addr >= FN_WINDOW_BASE + FN_WINDOW_SIZE)
        return 0xff;
    off = (uint32_t)(addr - FN_WINDOW_BASE);
    if (c->async)
        drain_pokes(c);
    data = a52_serve(c->view, off);
    if (!no_side_effects)
        access_cycle(c, off);
    return data;
}

void a52_fujinet_write(a52_fujinet_t *c, uint16_t addr, uint8_t value)
{
    (void)value;
    if (!c || !c->powered || addr < FN_WINDOW_BASE || addr >= FN_WINDOW_BASE + FN_WINDOW_SIZE)
        return;
    if (c->async)
        drain_pokes(c);
    access_cycle(c, (uint32_t)(addr - FN_WINDOW_BASE));
}

unsigned a52_fujinet_view_serial(const a52_fujinet_t *c)
{
    return c ? atomic_load((atomic_uint *)&c->serial) : 0;
}

void a52_fujinet_copy_window(a52_fujinet_t *c, uint8_t dst[0x8000])
{
    if (!c || !c->powered) {
        memset(dst, 0xff, 0x8000);
        return;
    }
    drain_pokes(c);
    for (unsigned i = 0; i < A52MAP_PAGES; i++)
        memcpy(dst + i * A52MAP_PAGE, c->view->page[i], A52MAP_PAGE);
}

void a52_fujinet_frame(a52_fujinet_t *c)
{
    if (c && c->async)
        drain_pokes(c);
}

/*-------------------------------------------------
    status
-------------------------------------------------*/

void a52_fujinet_status(a52_fujinet_t *c, a52_cart_status_t *out)
{
    const a52_view_t *v;
    unsigned live;

    memset(out, 0, sizeof *out);
    if (!c)
        return;
    pthread_mutex_lock(&c->loadlock);
    live = c->load.live;
    v = &c->load.view[live];
    out->powered = c->powered;
    out->mailbox = v->mailbox;
    out->link_up = fujitcp_active();
    out->link_off = c->link_off;
    out->claim = c->load.plan[live].claim;
    out->booted_game = c->served_other;
    out->armed = c->load.armed;
    out->staged = c->load.staged;
    out->staged_claim = c->load.staged && c->load.plan[live ^ 1u].claim;
    out->staged_config = c->load.staged && c->load.staged_config;
    out->mapper = v->kind;
    out->staged_mapper = c->load.plan[live ^ 1u].kind;
    out->nbanks = v->kind == A52MAP_SUPERCART ? v->nbanks : 0;
    out->bank[0] = v->bank[0];
    out->bank[1] = v->bank[1];
    out->live_size = c->live_size;
    out->live_crc = c->live_crc;
    out->staged_size = c->staged_size;
    out->staged_crc = c->staged_crc;
    out->swaps = c->load.swaps;
    pthread_mutex_unlock(&c->loadlock);
    out->boot_state = c->arena[FN_R_BOOT_STATE];
    out->boot_pct = c->arena[FN_R_BOOT_PCT];
    out->boot_err = c->arena[FN_R_BOOT_ERR];
    out->ackseq = c->arena[FN_R_ACKSEQ];
    out->status = c->arena[FN_R_STATUS];
    out->err = c->arena[FN_R_ERR];
    out->reply_cmd = c->arena[FN_R_REPLY_CMD];
    out->rxlen = (uint16_t)(c->arena[FN_R_RXLEN_LO] | c->arena[FN_R_RXLEN_HI] << 8);
    pthread_mutex_lock(&c->qlock);
    out->queue = (c->qhead + QUEUE_SIZE - c->qtail) % QUEUE_SIZE;
    pthread_mutex_unlock(&c->qlock);
}

void a52_fujinet_arena(a52_fujinet_t *c, uint8_t out[A52_FJ_ARENA_BYTES])
{
    if (!c) {
        memset(out, 0xff, A52_FJ_ARENA_BYTES);
        return;
    }
    memcpy(out, c->arena, A52_FJ_ARENA_BYTES);
}

uint8_t a52_fujinet_view_byte(a52_fujinet_t *c, uint32_t offset)
{
    if (!c || !c->powered || offset >= A52MAP_WINDOW)
        return 0xff;
    return a52_serve(c->view, offset);
}

void a52_fujinet_view_poke(a52_fujinet_t *c, uint32_t offset, uint8_t data)
{
    uint8_t *p;

    if (!c || !c->powered || offset >= A52MAP_WINDOW)
        return;
    p = (uint8_t *)c->view->page[(offset >> 11) & 15u];
    p[offset & (A52MAP_PAGE - 1u)] = data;
}
