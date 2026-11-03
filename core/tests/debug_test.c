/* The debugger engine on a 5200 with the FujiNet cartridge: a little program
   poked into RAM, then break, step, step over and out, frame step,
   watchpoints, conditions, a scanline break, a CIM stop (attached and not),
   the A assembler, labels (5200 hardware, mirrors, the cartridge's arena),
   the budget, the cartridge state and the side-effect-free reads of its
   mailbox, a power cycle at a breakpoint. No ROM (the built-in Altirra OS),
   no network (FujiNet off), the cartridge in sync mode; no tight timing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "a52session.h"
#include "a52debug.h"
#include "test_tmpdir.h"

static void test_sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); \
    test_sleep_ms(200); _exit(1); } } while (0)

/* Waits for n more frames (generous: a slow runner). */
static int wait_frames(a52session *s, int n)
{
    uint64_t start = a52session_frame_count(s);
    int t;
    for (t = 0; t < 30000 && a52session_frame_count(s) < start + (uint64_t)n; t += 10)
        test_sleep_ms(10);
    return a52session_frame_count(s) >= start + (uint64_t)n;
}

/* $0600 LDA #$05 / JSR $0610 / STA $0700 / LDA $0701 / INX / JMP $0600
   $0610 CLC / ADC #$01 / RTS                     $0620 $02 (a CIM) */
static const uint8_t prog[] = {
    0xA9, 0x05, 0x20, 0x10, 0x06, 0x8D, 0x00, 0x07, 0xAD, 0x01, 0x07, 0xE8, 0x4C, 0x00, 0x06,
    0xEA, 0x18, 0x69, 0x01, 0x60};

static a52debug_cpu cpu(a52debug *d)
{
    a52debug_cpu c;
    a52debug_cpu_get(d, &c);
    return c;
}

/* Waits for a stop; its reason into why. */
static int wait_stop(a52debug *d, char *why, int whysz)
{
    int t;
    for (t = 0; t < 3000 && !a52debug_is_stopped(d); t += 5)
        test_sleep_ms(5);
    a52debug_stop_reason(d, why, whysz, NULL);
    return a52debug_is_stopped(d);
}

/* Parks the machine in the program: PC, NMIs and IRQs off. */
static void enter_prog(a52debug *d, int pc)
{
    a52debug_write(d, 0xD40E, 0x00);   /* NMIEN */
    a52debug_cpu_set(d, A52_FLAG_I, 1);
    a52debug_cpu_set(d, A52_REG_PC, pc);
}

int main(void)
{
    char base[512], cfg[600], data[600];
    a52session_paths sp;
    a52session_start_opts o;
    a52session *s;
    a52debug *d;
    char why[128], out[8192], msg[256];
    uint8_t b[4];
    int i, id, x0, max, rc;
    a52debug_line lines[8];
    a52debug_history hist[16];
    a52_fuji_packet pk[4];

    test_tmpdir(base, sizeof base, "debug");
    snprintf(cfg, sizeof cfg, "%s/config", base);
    snprintf(data, sizeof data, "%s/data", base);
    memset(&sp, 0, sizeof sp);
    sp.config_dir = cfg;
    sp.data_dir = data;
    sp.fujinet_lib = "";
    s = a52session_new(&sp);
    CHECK(s != NULL, "session");
    a52session_default_opts(s, &o);
    o.enable_fujinet = 0;
    o.enable_audio = 0;
    o.enable_gamepad = 0;
    o.sync_cart = 1;
    o.cart_path = "";
    rc = a52session_start(s, &o);
    CHECK(rc == 0, "start: %s", a52session_last_error(s));
    CHECK(wait_frames(s, 20), "the console runs");
    d = a52session_debugger(s);

    /* attach: stops */
    a52debug_attach(d);
    CHECK(wait_stop(d, why, sizeof why), "attach stops");
    CHECK(!strcmp(why, "Paused"), "reason %s", why);
    for (i = 0; i < (int)sizeof prog; i++)
        a52debug_write(d, (uint16_t)(0x0600 + i), prog[i]);
    a52debug_write(d, 0x0620, 0x02);
    enter_prog(d, 0x0600);

    /* step, step over a JSR, step into it and out */
    a52debug_step(d);
    CHECK(wait_stop(d, why, sizeof why), "step");
    CHECK(cpu(d).pc == 0x0602 && cpu(d).a == 5 && !strcmp(why, "Step"), "step: %04X %s", cpu(d).pc, why);
    a52debug_step_over(d);
    CHECK(wait_stop(d, why, sizeof why), "over");
    CHECK(cpu(d).pc == 0x0605 && cpu(d).a == 6, "over: %04X A=%02X %s", cpu(d).pc, cpu(d).a, why);
    a52debug_step(d);
    CHECK(wait_stop(d, why, sizeof why) && cpu(d).pc == 0x0608, "sta");
    a52debug_read(d, 0x0700, b, 1);
    CHECK(b[0] == 6, "STA wrote %02X", b[0]);
    a52debug_cpu_set(d, A52_REG_PC, 0x0602);
    a52debug_step(d);
    CHECK(wait_stop(d, why, sizeof why) && cpu(d).pc == 0x0610, "into JSR: %04X", cpu(d).pc);
    a52debug_step_out(d);
    CHECK(wait_stop(d, why, sizeof why), "out");
    CHECK(cpu(d).pc == 0x0605 && !strcmp(why, "Returned"), "out: %04X %s", cpu(d).pc, why);

    /* an execution breakpoint, hit twice: resuming steps off it */
    id = a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, 0x060B, 0x060B, "", msg, sizeof msg);
    CHECK(id == 1, "bp id %d %s", id, msg);
    CHECK(a52debug_breakpoint_check(d, 0x060B) == 1, "check");
    a52debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "bp");
    CHECK(cpu(d).pc == 0x060B && !strcmp(why, "Breakpoint 1"), "bp: %04X %s", cpu(d).pc, why);
    x0 = cpu(d).x;
    a52debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "bp again");
    CHECK(cpu(d).pc == 0x060B && cpu(d).x == ((x0 + 1) & 0xff), "bp again: %04X X=%02X (%02X)",
          cpu(d).pc, cpu(d).x, x0);
    a52debug_disassemble(d, 0x060B, lines, 1, NULL);
    CHECK(lines[0].has_breakpoint && lines[0].is_pc && strstr(lines[0].disasm, "INX"),
          "line: %s", lines[0].disasm);
    a52debug_breakpoint_toggle(d, 0x060B);
    CHECK(a52debug_breakpoint_check(d, 0x060B) == 0, "toggled off");

    /* watchpoints: a write, then a read */
    id = a52debug_breakpoint_add(d, A52DEBUG_BP_WRITE, 0x0700, 0x0700, "", msg, sizeof msg);
    CHECK(id == 1001, "wp id %d", id);
    a52debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "write wp");
    CHECK(cpu(d).pc == 0x0605 && strstr(why, "Watchpoint 1001: write $0700"), "write wp: %04X %s",
          cpu(d).pc, why);
    a52debug_breakpoint_remove(d, id);
    id = a52debug_breakpoint_add(d, A52DEBUG_BP_READ, 0x0701, 0x0701, "", msg, sizeof msg);
    a52debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "read wp");
    CHECK(cpu(d).pc == 0x0608 && strstr(why, "read $0701"), "read wp: %04X %s", cpu(d).pc, why);
    a52debug_breakpoint_clear(d);

    /* a condition */
    CHECK(a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, 0x060B, 0x060B, "X=", msg, sizeof msg) < 0,
          "bad condition");
    a52debug_cpu_set(d, A52_REG_X, 0);
    id = a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, 0x060B, 0x060B, "X=$10 && MEM[$0700]=6", msg, sizeof msg);
    CHECK(id > 0, "cond add: %s", msg);
    a52debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "cond");
    CHECK(cpu(d).pc == 0x060B && cpu(d).x == 0x10, "cond: %04X X=%02X", cpu(d).pc, cpu(d).x);
    a52debug_breakpoint_clear(d);

    /* history: the loop just ran */
    rc = a52debug_history_get(d, hist, 16);
    CHECK(rc == 16, "history %d", rc);
    for (i = 13; i < 16 && !(hist[i].pc == 0x0608 && strstr(hist[i].disasm, "LDA")); i++)
        ;
    CHECK(i < 16, "history: %04X %s / %04X %s", hist[14].pc, hist[14].disasm, hist[15].pc,
          hist[15].disasm);

    /* frame steps */
    a52debug_frame(d);
    CHECK(wait_stop(d, why, sizeof why), "frame");
    {
        a52debug_cpu c = cpu(d), c2;
        CHECK(c.scanline == 248 && !strcmp(why, "Frame"), "frame: line %d %s", c.scanline, why);
        a52debug_frame(d);
        CHECK(wait_stop(d, why, sizeof why), "frame 2");
        c2 = cpu(d);
        CHECK(c2.scanline == 248 && c2.frame == c.frame + 1, "frame 2: line %d frame %u (%u)",
              c2.scanline, c2.frame, c.frame);
    }

    /* the console: a scanline break, CONT */
    rc = a52debug_command(d, "BLINE 100", out, sizeof out);
    CHECK(rc == A52DBG_CMD_OK && strstr(out, "scanline 100"), "BLINE: %s", out);
    rc = a52debug_command(d, "CONT", out, sizeof out);
    CHECK(rc == A52DBG_CMD_RESUMED, "CONT %d", rc);
    CHECK(wait_stop(d, why, sizeof why), "scanline");
    CHECK(cpu(d).scanline == 100 && !strcmp(why, "Scanline 100"), "scanline: %d %s", cpu(d).scanline, why);

    /* the A assembler asks for lines */
    rc = a52debug_command(d, "A 0640", out, sizeof out);
    CHECK(rc == A52DBG_CMD_NEEDS_INPUT && strstr(out, "0640: "), "A: %d %s", rc, out);
    {
        a52debug_cpu c = cpu(d);   /* inspection works meanwhile */
        CHECK(c.scanline == 100, "cpu while assembling");
    }
    rc = a52debug_command(d, "LDA #$42", out, sizeof out);
    CHECK(rc == A52DBG_CMD_NEEDS_INPUT && strstr(out, "0642: "), "A line: %d %s", rc, out);
    rc = a52debug_command(d, "", out, sizeof out);
    CHECK(rc == A52DBG_CMD_OK, "A end %d", rc);
    a52debug_read(d, 0x0640, b, 2);
    CHECK(b[0] == 0xA9 && b[1] == 0x42, "assembled %02X %02X", b[0], b[1]);

    /* the assembler given up by a detach: the console still works after */
    rc = a52debug_command(d, "A 0660", out, sizeof out);
    CHECK(rc == A52DBG_CMD_NEEDS_INPUT, "A again %d", rc);
    a52debug_detach(d);
    a52debug_attach(d);
    CHECK(wait_stop(d, why, sizeof why), "attach after giving up");
    rc = a52debug_command(d, "BLINE 120", out, sizeof out);
    CHECK(rc == A52DBG_CMD_OK && strstr(out, "scanline 120"), "console after detach: %d [%s]", rc, out);
    a52debug_command(d, "BLINE 999", out, sizeof out);
    enter_prog(d, 0x0600);

    /* labels: atari800's 5200 table, mirrors folded, the cartridge's arena */
    CHECK(a52debug_label_address(d, "SDMCTL") == 0x0007, "SDMCTL");
    CHECK(a52debug_label_address(d, "fuji_regsel") == 0xB500, "FUJI_REGSEL");
    a52debug_address_label(d, 0xE80A, why, sizeof why);
    CHECK(!strcmp(why, "RANDOM"), "label %s", why);
    a52debug_address_label(d, 0xE81A, why, sizeof why);   /* POKEY's mirror */
    CHECK(!strcmp(why, "RANDOM"), "mirror label %s", why);
    a52debug_address_label(d, 0xC41F, why, sizeof why);   /* GTIA's mirror */
    CHECK(!strcmp(why, "CONSOL"), "gtia mirror label %s", why);
    a52debug_address_label(d, 0xB5F2, why, sizeof why);
    CHECK(!strcmp(why, "FUJI_SWAP"), "arena label %s", why);
    a52debug_write(d, 0x0650, 0xAD);   /* LDA $E80A */
    a52debug_write(d, 0x0651, 0x0A);
    a52debug_write(d, 0x0652, 0xE8);
    a52debug_disassemble(d, 0x0650, lines, 1, NULL);
    CHECK(strstr(lines[0].comment, "RANDOM") && strstr(lines[0].disasm, "LDA $E80A")
          && !strcmp(lines[0].bytes, "AD 0A E8"), "dis: [%s] [%s] [%s]", lines[0].bytes, lines[0].disasm,
          lines[0].comment);
    {
        char lab[1100];
        FILE *fp;
        snprintf(lab, sizeof lab, "%s/game.lbl", a52session_data_path(s));
        fp = fopen(lab, "w");
        CHECK(fp != NULL, "lbl");
        fputs("al 000600 .main\nal C:0610 .addone\n", fp);
        fclose(fp);
        CHECK(a52debug_load_symbols(d, lab, msg, sizeof msg) == 2, "symbols: %s", msg);
        CHECK(a52debug_label_address(d, "addone") == 0x0610, "addone");
        a52debug_disassemble(d, 0x0602, lines, 1, NULL);
        CHECK(strstr(lines[0].comment, "addone"), "dis jsr: %s ;%s", lines[0].disasm, lines[0].comment);
        CHECK(a52debug_label_address(d, "SDMCTL") == 0x0007, "5200 labels kept");
    }

    /* the budget: 10 breakpoints take 19 entries; an 11th does not fit */
    for (i = 0; i < 10; i++)
        CHECK(a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, (uint16_t)(0x5000 + i), (uint16_t)(0x5000 + i),
                                       "", msg, sizeof msg) > 0, "bp %d: %s", i, msg);
    CHECK(a52debug_breakpoint_budget(d, &max) == 19 && max == 20, "budget");
    CHECK(a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, 0x5100, 0x5100, "", msg, sizeof msg) < 0
          && strstr(msg, "20"), "over budget: %s", msg);
    a52debug_breakpoint_clear(d);

    /* chips */
    {
        a52debug_antic an;
        a52debug_gtia gt;
        a52debug_pokey pk0, pk1;
        static uint32_t pal[A52DEBUG_PALETTE_WIDTH * A52DEBUG_PALETTE_HEIGHT];
        a52debug_antic_get(d, &an);
        CHECK(an.dlist != 0 && an.dl_dma, "antic dlist %04X dmactl %02X", an.dlist, an.dmactl);
        a52debug_dlist_text(d, out, sizeof out);
        CHECK(strstr(out, "JVB") && strstr(out, "LMS"), "dlist:\n%s", out);
        a52debug_gtia_get(d, &gt);
        a52debug_pokey_get(d, 0, &pk0);
        a52debug_pokey_get(d, 1, &pk1);
        CHECK(pk1.audctl == 0 && pk1.second == 0 && pk1.irqen == 0, "no second POKEY");
        CHECK(a52debug_palette_image(d, pal) == 1 && (pal[0] >> 24) == 0xff, "palette");
    }

    /* the cartridge: CONFIG is a 32K ROM client with the mailbox live */
    {
        a52debug_map m, m2;
        uint8_t ar[4];
        a52debug_map_get(d, &m);
        CHECK(m.cart_powered && !strcmp(m.mapper_name, "ROM") && m.mailbox && m.claim && m.nbanks == 0
              && !m.booted_game && m.link_off && !m.link_up && m.swaps == 0 && m.live_size > 0,
              "CONFIG: powered %d mapper %s mailbox %d claim %d booted %d link_off %d swaps %d",
              m.cart_powered, m.mapper_name, m.mailbox, m.claim, m.booted_game, m.link_off, m.swaps);
        CHECK(m.bios[0] != 0, "bios name");
        CHECK(a52debug_cart_info(d, out, sizeof out) > 0 && strstr(out, "CONFIG"), "cart info: %s", out);
        CHECK(m.arena[0x409] == 'F' && m.arena[0x40A] == 'N', "arena magic %02X %02X", m.arena[0x409],
              m.arena[0x40A]);
        CHECK(m.arena[0x5F0] == 0x6C && m.arena[0x5F1] == 0xFC && m.arena[0x5F2] == 0xFF, "arena stub");
        /* no side effects: the hotspots (REGSEL, REGDATA, TX and the swap read) are only read */
        for (i = 0; i < 0x300; i++)
            a52debug_read(d, (uint16_t)(0xB500 + i), b, 1);
        a52debug_read(d, 0xB5F2, b, 1);
        a52debug_read(d, 0xB5F0, ar, 3);
        CHECK(ar[0] == 0x6C && ar[1] == 0xFC && ar[2] == 0xFF, "stub through the CPU view: %02X %02X %02X",
              ar[0], ar[1], ar[2]);
        a52debug_read(d, 0xB409, ar, 2);
        CHECK(ar[0] == 'F' && ar[1] == 'N', "magic through the CPU view");
        a52debug_map_get(d, &m2);
        CHECK(m2.swaps == 0 && m2.queue_depth == 0 && !m2.armed && m2.ackseq == m.ackseq
              && m2.status == m.status && !memcmp(m2.arena, m.arena, sizeof m.arena),
              "reading the arena changed the cartridge (swaps %d queue %u armed %d)", m2.swaps,
              m2.queue_depth, m2.armed);
        /* a poke into the cartridge window is the view's own, not a cart cycle */
        a52debug_read(d, 0x4000, b, 1);
        a52debug_write(d, 0x4000, (uint8_t)(b[0] ^ 0xFF));
        a52debug_read(d, 0x4000, b + 1, 1);
        CHECK(b[1] == (uint8_t)(b[0] ^ 0xFF), "poked cart byte %02X", b[1]);
        a52debug_write(d, 0x4000, b[0]);
        a52debug_map_get(d, &m2);
        CHECK(m2.queue_depth == 0 && m2.swaps == 0, "poke made no cycle");
        CHECK(a52debug_save(d, "arena", "/dev/null", msg, sizeof msg) == 0, "save arena");
        CHECK(a52debug_save(d, "cart", "/dev/null", msg, sizeof msg) == 0, "save cart");
    }

    /* FujiNet is off: the state says so, and the trace is empty but works */
    CHECK(a52debug_fujinet_state(d, out, sizeof out) > 0 && strstr(out, "Link") && strstr(out, "off"),
          "fujinet state: %s", out);
    CHECK(a52debug_fujinet_trace(d, pk, 4) == 0, "no packets with FujiNet off");
    a52debug_fujinet_trace_clear(d);

    /* stopped at a breakpoint, Power Cycle (a frame job) must still run:
       the breakpoint would otherwise fire for ever as the park lets go */
    enter_prog(d, 0x0600);
    id = a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, 0x0605, 0x0605, "", msg, sizeof msg);
    CHECK(id > 0, "bp for the power cycle");
    a52debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why) && cpu(d).pc == 0x0605, "bp before power cycle");
    CHECK(a52session_power_cycle(s) == 0, "power cycle at a breakpoint");
    CHECK(wait_frames(s, 20), "running after a power cycle at a breakpoint");
    a52debug_breakpoint_clear(d);
    a52debug_stop(d);
    CHECK(wait_stop(d, why, sizeof why), "stopped again");
    for (i = 0; i < (int)sizeof prog; i++)
        a52debug_write(d, (uint16_t)(0x0600 + i), prog[i]);
    a52debug_write(d, 0x0620, 0x02);

    /* a CIM, attached: a stop */
    a52debug_cpu_set(d, A52_REG_PC, 0x0620);
    a52debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "cim");
    CHECK(!strncmp(why, "CIM", 3), "cim: %s", why);

    /* ...and detached: parked, with a notice; Power Cycle gets it going */
    a52debug_cpu_set(d, A52_REG_PC, 0x0620);
    a52debug_detach(d);
    for (i = 0; i < 3000 && !a52debug_is_stopped(d); i += 5)
        test_sleep_ms(5);
    CHECK(a52debug_is_stopped(d), "detached cim parks");
    CHECK(a52session_take_notice(s, msg, sizeof msg) > 0 && strstr(msg, "crashed"), "notice: %s", msg);
    CHECK(a52session_power_cycle(s) == 0, "power cycle");
    CHECK(wait_frames(s, 20), "running after the crash");
    CHECK(!a52debug_is_stopped(d), "running");

    /* quitting with the PC on an execution breakpoint must not hang */
    a52debug_attach(d);
    CHECK(wait_stop(d, why, sizeof why), "attach before quitting");
    enter_prog(d, 0x0600);
    for (i = 0; i < (int)sizeof prog; i++)
        a52debug_write(d, (uint16_t)(0x0600 + i), prog[i]);
    a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, 0x0600, 0x0600, "", msg, sizeof msg);
    a52session_free(s);
    printf("debug ok\n");
    return 0;
}
