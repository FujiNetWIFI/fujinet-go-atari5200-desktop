/*
 * debug.cpp -- the debugger engine (a52debug.h) over atari800's monitor.
 *
 * atari800 breaks into its monitor through PLATFORM_Exit(TRUE) from inside
 * CPU_GO; A52Host parks the emulation thread there and runs "Any" jobs
 * while parked, so everything below runs on the emulation thread with the
 * registers in CPU_reg*. Breakpoints are encoded into atari800's condition
 * table (monitor.h), which it checks before each instruction unless it is
 * single-stepping: resuming from a table breakpoint steps over it first.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "a52debug.h"
#include "fujinet_cart.h"
#include "fujitcp_host.h"
#include "fujitrace.h"
#include "session_impl.h"

#include "fuji_mailbox.h"

extern "C" {
#include "antic.h"
#include "atari.h"
#include "cpu.h"
#include "gtia.h"
#include "memory.h"
#include "monitor.h"
#include "pokey.h"
#include "a8video.h"
}

namespace {

enum class Intent { None, Pause, Step, Over, Out, Frame, RunTo, Monitor };

struct Bp
{
	int id;
	int type;
	uint16_t start, end;
	bool enabled;
	std::string condition;
};

// One parsed condition: a register or memory byte against a value.
struct Cond
{
	int what;       // MONITOR_BREAKPOINT_A ... or MEMORY
	int ops;        // LESS | EQUAL | GREATER: any of them holding passes
	int value;
	int m_addr;
};

constexpr int FrameLine = 248;      // ANTIC's first line of vertical blank
constexpr int YposOff = 999;
constexpr uint16_t BreakAddrOff = 0xd000;
constexpr long ConsoleKeep = 256 * 1024;

} // namespace

struct a52debug
{
	a52session* s = nullptr;
	SessionImpl* im = nullptr;
	std::atomic<bool> attached { false };
	std::atomic<unsigned> generation { 1 };

	std::mutex mtx;
	std::vector<Bp> bps;
	int nextBp = 1, nextWp = 1001;
	std::string reason;
	int reasonPc = -1;
	Intent intent = Intent::None;
	uint16_t runTo = 0;
	bool atTableBp = false;      // stopped by the condition table
	bool skipping = false;       // stepping off it
	bool frameChase = false;     // frame step begun on the frame line

	// the monitor console: written and read on the emulation thread
	std::mutex cmdMtx;
	std::condition_variable cmdCv;
	FILE* out = nullptr;
	long outRead = 0;
	bool cmdRunning = false, cmdDone = false, waitingInput = false, haveInput = false;
	int cmdRc = 0;
	std::string input;

	// the FujiNet trace: the cartridge's BoIP socket bytes, from session
	// start (the cartridge's worker thread, or the emulation thread in sync
	// mode, feeds it)
	std::mutex fujiMtx;
	fujitrace fuji;
};

namespace {

a52debug* g_debug;   // for the FujiNet tap and the monitor's input hook
UWORD g_dlistStart;   // the display list this frame began with

// ANTIC_dlist is where ANTIC is fetching: the list's start only once the
// JVB has run (vertical blank, where the OS also rewrites it).
UWORD dlist_start()
{
	return ANTIC_ypos >= FrameLine ? ANTIC_dlist : g_dlistStart;
}

std::string fmt(const char* f, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, f);
	vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	return buf;
}

// ---- labels: atari800's own 5200 table, plus the cartridge's and the vectors --

struct Label { const char* name; uint16_t addr; };

const Label kExtraLabels[] = {
	{"FUJI_REPLY", 0xB000}, {"FUJI_STATUS", 0xB400}, {"FUJI_ACKSEQ", 0xB400},
	{"FUJI_LINK", 0xB401}, {"FUJI_ERR", 0xB402}, {"FUJI_REPLY_CMD", 0xB403},
	{"FUJI_RXLEN", 0xB404}, {"FUJI_BOOT_STATE", 0xB406}, {"FUJI_BOOT_PCT", 0xB407},
	{"FUJI_BOOT_ERR", 0xB408}, {"FUJI_MAGIC", 0xB409}, {"FUJI_MODE", 0xB412},
	{"FUJI_MAPPER", 0xB414}, {"FUJI_ARMED", 0xB41C}, {"FUJI_STAGED", 0xB41D},
	{"FUJI_SWAPS", 0xB423},
	{"FUJI_REGSEL", 0xB500}, {"FUJI_STUB", 0xB5F0}, {"FUJI_SWAP", 0xB5F2},
	{"FUJI_REGDATA", 0xB600}, {"FUJI_TX", 0xB700}, {"FUJI_CLAIM", 0xBFE0},
	{"NMI_VECTOR", 0xFFFA}, {"RESET_VECTOR", 0xFFFC}, {"IRQ_VECTOR", 0xFFFE},
	{nullptr, 0}};

// A mirror folds to its base: the 5200's GTIA decodes only A0-A4 ($C000-$CFFF),
// ANTIC A0-A3 ($D400-$D7FF), POKEY A0-A3 ($E800-$EFFF).
uint16_t fold_mirror(uint16_t a)
{
	if (a >= 0xC000 && a < 0xD000)
		return (uint16_t)(0xC000 | (a & 0x1f));
	if (a >= 0xD400 && a < 0xD800)
		return (uint16_t)(0xD400 | (a & 0x0f));
	if (a >= 0xE800 && a < 0xF000)
		return (uint16_t)(0xE800 | (a & 0x0f));
	return a;
}

bool ci_equal(const char* a, const char* b)
{
	for (; *a && *b; a++, b++)
		if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
			return false;
	return !*a && !*b;
}

// Emulation thread. Exact name for an address, "" if none.
std::string label_for(uint16_t a, int is_write)
{
	const char* l = MONITOR_LabelFor(a, is_write);
	if (!l && fold_mirror(a) != a)
		l = MONITOR_LabelFor(fold_mirror(a), is_write);
	if (l)
		return l;
	for (const Label* e = kExtraLabels; e->name; e++)
		if (e->addr == a)
			return e->name;
	return "";
}

// ---- breakpoints into atari800's table ------------------------------------

bool parse_conditions(const std::string& text, std::vector<Cond>& out, std::string& err)
{
	size_t pos = 0;
	while (pos < text.size()) {
		size_t amp = text.find("&&", pos);
		std::string part = text.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
		pos = amp == std::string::npos ? text.size() : amp + 2;
		part.erase(std::remove(part.begin(), part.end(), ' '), part.end());
		if (part.empty())
			continue;
		for (auto& c : part)
			c = (char)toupper((unsigned char)c);
		Cond c {0, 0, 0, 0};
		size_t i = 0;
		if (part.rfind("MEM[", 0) == 0) {
			size_t close = part.find(']');
			if (close == std::string::npos) {
				err = "MEM[addr] needs its ]";
				return false;
			}
			std::string a = part.substr(4, close - 4);
			if (!a.empty() && a[0] == '$')
				a.erase(0, 1);
			c.what = MONITOR_BREAKPOINT_MEMORY;
			c.m_addr = (int)strtol(a.c_str(), nullptr, 16);
			i = close + 1;
		}
		else {
			switch (part[0]) {
			case 'A': c.what = MONITOR_BREAKPOINT_A; break;
			case 'X': c.what = MONITOR_BREAKPOINT_X; break;
			case 'Y': c.what = MONITOR_BREAKPOINT_Y; break;
			case 'S': c.what = MONITOR_BREAKPOINT_S; break;
			default:
				err = "conditions are on A, X, Y, S or MEM[addr]";
				return false;
			}
			i = 1;
		}
		std::string op;
		while (i < part.size() && strchr("<>=!", part[i]))
			op += part[i++];
		if (op == "=" || op == "==")
			c.ops = MONITOR_BREAKPOINT_EQUAL;
		else if (op == "<")
			c.ops = MONITOR_BREAKPOINT_LESS;
		else if (op == ">")
			c.ops = MONITOR_BREAKPOINT_GREATER;
		else if (op == "<=")
			c.ops = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL;
		else if (op == ">=")
			c.ops = MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL;
		else if (op == "!=" || op == "<>")
			c.ops = MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_GREATER;
		else {
			err = "compare with =, !=, <, >, <= or >=";
			return false;
		}
		std::string v = part.substr(i);
		int base = 10;
		if (!v.empty() && v[0] == '$') {
			v.erase(0, 1);
			base = 16;
		}
		char* end = nullptr;
		long value = v.empty() ? -1 : strtol(v.c_str(), &end, base);
		if (v.empty() || *end || value < 0 || value > 0xffff) {
			err = "compare with a number ($10 is hex)";
			return false;
		}
		c.value = (int)value;
		out.push_back(c);
	}
	return true;
}

// The entries a breakpoint needs, appended to rows.
void encode(const Bp& b, std::vector<MONITOR_breakpoint_cond>& rows)
{
	auto add = [&](int cond, int value, int m_addr) {
		MONITOR_breakpoint_cond r;
		r.enabled = 1;
		r.condition = (UWORD)cond;
		r.value = (UWORD)value;
		r.m_addr = (UWORD)m_addr;
		rows.push_back(r);
	};
	int what = b.type == A52DEBUG_BP_EXEC ? MONITOR_BREAKPOINT_PC
	           : b.type == A52DEBUG_BP_READ ? MONITOR_BREAKPOINT_READ
	           : b.type == A52DEBUG_BP_WRITE ? MONITOR_BREAKPOINT_WRITE
	                                          : MONITOR_BREAKPOINT_ACCESS;
	if (!rows.empty())
		add(MONITOR_BREAKPOINT_OR, 0, 0);
	if (b.start == b.end)
		add(what | MONITOR_BREAKPOINT_EQUAL, b.start, 0);
	else {
		add(what | MONITOR_BREAKPOINT_GREATER | MONITOR_BREAKPOINT_EQUAL, b.start, 0);
		add(what | MONITOR_BREAKPOINT_LESS | MONITOR_BREAKPOINT_EQUAL, b.end, 0);
	}
	std::vector<Cond> conds;
	std::string err;
	parse_conditions(b.condition, conds, err);
	for (auto& c : conds)
		add(c.what | c.ops, c.value, c.m_addr);
}

std::vector<MONITOR_breakpoint_cond> encode_all(const std::vector<Bp>& bps)
{
	std::vector<MONITOR_breakpoint_cond> rows;
	for (auto& b : bps)
		if (b.enabled)
			encode(b, rows);
	return rows;
}

// Emulation thread: the table from the list (empty while detached).
void arm(a52debug* d)
{
	std::vector<MONITOR_breakpoint_cond> rows;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		if (d->attached.load())
			rows = encode_all(d->bps);
	}
	int n = (int)std::min<size_t>(rows.size(), MONITOR_BREAKPOINT_TABLE_MAX);
	for (int i = 0; i < n; i++)
		MONITOR_breakpoint_table[i] = rows[i];
	MONITOR_breakpoint_table_size = n;
	MONITOR_breakpoints_enabled = TRUE;
}

void clear_breaks()
{
	MONITOR_break_step = FALSE;
	MONITOR_break_ret = FALSE;
	MONITOR_break_addr = BreakAddrOff;
	ANTIC_break_ypos = YposOff;
}

void bump(a52debug* d)
{
	d->generation.fetch_add(1);
}

// The address the instruction at pc reads or writes, as cpu.c works it out
// for the table.
int effective_address(uint16_t pc, int* optype)
{
	UBYTE insn = MEMORY_SafeGetByte(pc);
	UBYTE ot = MONITOR_optype6502[insn];
	auto byte = [&](int off) { return MEMORY_SafeGetByte((UWORD)(pc + off)); };
	auto word = [&](int off) { return byte(off) | (byte(off + 1) << 8); };
	auto zword = [](int zp) { return MEMORY_SafeGetByte((UWORD)zp) | (MEMORY_SafeGetByte((UWORD)((zp + 1) & 0xff)) << 8); };
	*optype = ot;
	switch (ot >> 4) {
	case 1: return word(1);
	case 2: return byte(1);
	case 3: return (word(1) + CPU_regX) & 0xffff;
	case 4: return (word(1) + CPU_regY) & 0xffff;
	case 5: return zword((byte(1) + CPU_regX) & 0xff);
	case 6: return (zword(byte(1)) + CPU_regY) & 0xffff;
	case 7: return (byte(1) + CPU_regX) & 0xff;
	case 8: return (byte(1) + CPU_regY) & 0xff;
	default: return -1;
	}
}

// Which of ours the table fired for (under d->mtx).
std::string table_reason(a52debug* d, uint16_t pc)
{
	for (auto& b : d->bps)
		if (b.enabled && b.type == A52DEBUG_BP_EXEC && pc >= b.start && pc <= b.end)
			return fmt("Breakpoint %d", b.id);
	int optype = 0;
	int ea = effective_address(pc, &optype);
	for (auto& b : d->bps) {
		if (!b.enabled || b.type == A52DEBUG_BP_EXEC || ea < b.start || ea > b.end)
			continue;
		bool reads = (optype & 4) && (b.type & A52DEBUG_BP_READ);
		bool writes = (optype & 8) && (b.type & A52DEBUG_BP_WRITE);
		if (reads || writes)
			return fmt("Watchpoint %d: %s $%04X", b.id, writes ? "write" : "read", ea);
	}
	return "Breakpoint";
}

// ---- the FujiNet trace ---------------------------------------------------------

// The transport's thread: every frame the cartridge sent (dir 1) or read
// (dir 0) on its BoIP socket.
void fuji_tap(void* user, int dir, const uint8_t* buf, size_t len)
{
	a52debug* d = static_cast<a52debug*>(user);
	std::lock_guard<std::mutex> lock(d->fujiMtx);
	fujitrace_feed(&d->fuji, dir, buf, len);
}

// ---- the monitor's input -----------------------------------------------------

// Emulation thread, inside a monitor command (A, the assembler): waits for
// the next a52debug_command line, running other inspection jobs meanwhile.
int gets_hook(char* buffer, size_t size, const char* prompt)
{
	a52debug* d = g_debug;
	if (!d)
		return 0;
	if (prompt && *prompt) {
		fputs(prompt, mon_output);
		fflush(mon_output);
	}
	std::unique_lock<std::mutex> lock(d->cmdMtx);
	d->waitingInput = true;
	d->cmdCv.notify_all();
	while (!d->haveInput && d->attached.load() && !d->im->host.Quitting()) {
		d->cmdCv.wait_for(lock, std::chrono::milliseconds(20));
		if (!d->haveInput) {
			lock.unlock();
			d->im->host.RunParkedJobs();
			lock.lock();
		}
	}
	d->waitingInput = false;
	if (!d->haveInput)
		return 0;
	d->haveInput = false;
	snprintf(buffer, size, "%s", d->input.c_str());
	return 1;
}

void console_open(a52debug* d)
{
	if (!d->out) {
		d->out = tmpfile();
		d->outRead = 0;
	}
	if (d->out)
		mon_output = d->out;
}

// Emulation thread: the console text written since the last read.
std::string console_take(a52debug* d)
{
	std::string text;
	if (!d->out)
		return text;
	fflush(d->out);
	fseek(d->out, d->outRead, SEEK_SET);
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, d->out)) > 0)
		text.append(buf, n);
	fseek(d->out, 0, SEEK_END);
	d->outRead = ftell(d->out);
	if (d->outRead > ConsoleKeep) {
		fclose(d->out);
		d->out = nullptr;
		console_open(d);
	}
	return text;
}

} // namespace

// ---- stops -------------------------------------------------------------------

// Emulation thread, parked in PLATFORM_Exit(TRUE): why, and stay or go on.
void session_debug_stopped(struct a52session* s)
{
	SessionImpl* im = Impl(s);
	a52debug* d = static_cast<a52debug*>(s->debugger);
	CPU_GetStatus();
	uint16_t pc = CPU_regPC;
	bool attached = d && d->attached.load();

	if (CPU_cim_encountered) {
		std::string why = fmt("CIM: the CPU jammed on $%02X at $%04X", MEMORY_SafeGetByte(pc), pc);
		if (attached)
			MONITOR_Enter();
		CPU_cim_encountered = 0;
		clear_breaks();
		if (d) {
			std::lock_guard<std::mutex> lock(d->mtx);
			d->reason = why;
			d->reasonPc = pc;
			d->intent = Intent::None;
			d->atTableBp = d->skipping = d->frameChase = false;
			bump(d);
		}
		if (!attached)
			im->Notice("The 5200 crashed (" + why + "). Power Cycle, or open the Debugger.");
		return;   // stays parked
	}
	if (!attached) {
		clear_breaks();
		im->host.Resume();
		return;
	}
	std::string why;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		bool returned = d->intent == Intent::Out && MONITOR_break_ret && MONITOR_ret_nesting <= 0;
		if (d->skipping && !returned) {
			// stepped off a table breakpoint: on with whatever else is set
			d->skipping = false;
			MONITOR_break_step = FALSE;
			im->host.Resume();
			return;
		}
		d->skipping = false;
		if (d->frameChase && ANTIC_break_ypos == ANTIC_ypos) {
			d->frameChase = false;
			ANTIC_break_ypos = FrameLine;
			im->host.Resume();
			return;
		}
		if (MONITOR_break_step) {
			switch (d->intent) {
			case Intent::Pause: why = "Paused"; break;
			case Intent::Over: why = "Step over"; break;
			case Intent::Out: why = "Returned"; break;
			case Intent::Monitor: why = MONITOR_break_ret ? "Returned" : "Step"; break;
			default: why = "Step"; break;
			}
		}
		else if (ANTIC_break_ypos == ANTIC_ypos)
			why = d->intent == Intent::Frame ? "Frame" : fmt("Scanline %d", ANTIC_ypos);
		else if (MONITOR_break_addr == pc)
			why = d->intent == Intent::RunTo ? fmt("Run to $%04X", pc)
			      : d->intent == Intent::Over ? std::string("Step over")
			                                  : fmt("Breakpoint at $%04X", pc);
		else if (MONITOR_break_brk && MEMORY_SafeGetByte((UWORD)(pc - 1)) == 0x00)
			why = fmt("BRK at $%04X", (uint16_t)(pc - 1));
		else if (MONITOR_breakpoint_table_size > 0) {
			why = table_reason(d, pc);
			d->atTableBp = true;
		}
		else
			why = "Stopped";
		d->reason = why;
		d->reasonPc = pc;
		d->intent = Intent::None;
	}
	MONITOR_Enter();
	fflush(mon_output);
	clear_breaks();
	bump(d);
}

void session_debug_after_frame(void)
{
	g_dlistStart = ANTIC_dlist;
}

// Pause / Break-to-debugger: attaches, which stops.
void session_debug_pause(struct a52session* s)
{
	a52debug* d = a52debug_get(s);
	if (!d->attached.load())
		a52debug_attach(d);
	else
		a52debug_stop(d);
}

void session_debug_free(struct a52session* s)
{
	a52debug* d = static_cast<a52debug*>(s->debugger);
	if (!d)
		return;
	if (g_debug == d) {
		g_debug = nullptr;
		MONITOR_gets_hook = nullptr;
		mon_output = stdout;
	}
	fujitcp_set_trace(nullptr, nullptr);   // once it returns, no tap call is in flight
	if (d->out)
		fclose(d->out);
	delete d;
	s->debugger = nullptr;
}

a52debug* a52session_debugger(a52session* s)
{
	return a52debug_get(s);
}

a52debug* a52debug_get(a52session* s)
{
	if (!s->debugger) {
		a52debug* d = new a52debug;
		d->s = s;
		d->im = Impl(s);
		fujitrace_init(&d->fuji);
		s->debugger = d;
	}
	return static_cast<a52debug*>(s->debugger);
}

// The FujiNet trace is always on: it is cheap, and it is most useful about
// what happened before anyone thought to look.
void session_debug_trace_install(struct a52session* s)
{
	g_debug = a52debug_get(s);
	fujitcp_set_trace(fuji_tap, g_debug);
}

void a52debug_attach(a52debug* d)
{
	if (d->attached.exchange(true))
		return;
	g_debug = d;
	d->im->host.Call([d] {
		MONITOR_gets_hook = gets_hook;
		console_open(d);
		arm(d);
	}, A52Host::Kind::Any);
	bump(d);
	a52debug_stop(d);
}

void a52debug_detach(a52debug* d)
{
	if (!d->attached.exchange(false))
		return;
	{
		std::lock_guard<std::mutex> lock(d->cmdMtx);
		d->cmdCv.notify_all();   // a monitor waiting for input gives up
	}
	d->im->host.Call([d] {
		MONITOR_breakpoint_table_size = 0;
		clear_breaks();
		std::lock_guard<std::mutex> lock(d->mtx);
		d->atTableBp = d->skipping = d->frameChase = false;
		d->intent = Intent::None;
	}, A52Host::Kind::Any);
	if (d->im->host.Stopped())
		d->im->host.Resume();
	bump(d);
}

int a52debug_is_attached(a52debug* d)
{
	return d->attached.load() ? 1 : 0;
}

int a52debug_is_stopped(a52debug* d)
{
	return d->im->host.Stopped() ? 1 : 0;
}

// Sets the machine going under intent; setup arms its break (emulation
// thread, d->mtx held).
static void go(a52debug* d, Intent intent, const std::function<void()>& setup)
{
	d->im->host.Call([&] {
		std::lock_guard<std::mutex> lock(d->mtx);
		d->intent = intent;
		d->frameChase = false;
		setup();
		if (d->atTableBp && !MONITOR_break_step) {
			// the table would fire again before the instruction runs
			d->skipping = true;
			MONITOR_break_step = TRUE;
		}
		d->atTableBp = false;
	}, A52Host::Kind::Any);
	if (d->im->host.Stopped())
		d->im->host.Resume();
	bump(d);
}

void a52debug_stop(a52debug* d)
{
	if (d->im->host.Stopped() || !d->im->host.IsRunning())
		return;
	d->im->host.Call([d] {
		std::lock_guard<std::mutex> lock(d->mtx);
		d->intent = Intent::Pause;
		MONITOR_break_step = TRUE;
	}, A52Host::Kind::Any);
	if (A52Host::OnEmulationThread())
		return;   // it stops after this frame's next instruction
	for (int i = 0; i < 100 && !d->im->host.Stopped(); i++) {
		struct timespec ts = {0, 5000000};
		nanosleep(&ts, nullptr);
	}
}

void a52debug_resume(a52debug* d)
{
	go(d, Intent::None, [] {});
}

void a52debug_step(a52debug* d)
{
	go(d, Intent::Step, [] { MONITOR_break_step = TRUE; });
}

void a52debug_step_over(a52debug* d)
{
	go(d, Intent::Over, [] {
		UBYTE op = MEMORY_SafeGetByte(CPU_regPC);
		if (op == 0x20)   // JSR: to the instruction after it
			MONITOR_break_addr = (UWORD)(CPU_regPC + 3);
		else
			MONITOR_break_step = TRUE;
	});
}

void a52debug_step_out(a52debug* d)
{
	go(d, Intent::Out, [] {
		MONITOR_break_ret = TRUE;
		MONITOR_ret_nesting = 1;
	});
}

void a52debug_frame(a52debug* d)
{
	go(d, Intent::Frame, [d] {
		if (ANTIC_ypos == FrameLine) {
			// it would fire at once: past this line first
			d->frameChase = true;
			ANTIC_break_ypos = FrameLine + 1;
		}
		else
			ANTIC_break_ypos = FrameLine;
	});
}

void a52debug_run_to(a52debug* d, uint16_t addr)
{
	go(d, Intent::RunTo, [d, addr] {
		d->runTo = addr;
		MONITOR_break_addr = addr;
	});
}

void a52debug_break_on_brk(a52debug* d, int on)
{
	d->im->host.Call([on] { MONITOR_break_brk = on ? TRUE : FALSE; }, A52Host::Kind::Any);
	bump(d);
}

int a52debug_breaks_on_brk(a52debug* d)
{
	int on = 0;
	d->im->host.Call([&] { on = MONITOR_break_brk; }, A52Host::Kind::Any);
	return on;
}

int a52debug_stop_reason(a52debug* d, char* dst, int dstsz, int* address)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	if (address)
		*address = d->reasonPc;
	if (!d->im->host.Stopped())
		return snprintf(dst, (size_t)dstsz, "Running");
	return snprintf(dst, (size_t)dstsz, "%s", d->reason.c_str());
}

unsigned a52debug_generation(a52debug* d)
{
	return d->generation.load() + d->im->host.ParkSerial();
}

// ---- the monitor console -----------------------------------------------------

int a52debug_command(a52debug* d, const char* line, char* dst, int dstsz)
{
	if (dst && dstsz > 0)
		*dst = 0;
	if (!d->im->host.IsRunning()) {
		if (dst && dstsz > 0)
			snprintf(dst, (size_t)dstsz, "The 5200 is not running.\n");
		return A52DBG_CMD_OK;
	}
	std::unique_lock<std::mutex> lock(d->cmdMtx);
	if (d->waitingInput) {
		d->input = line ? line : "";
		d->haveInput = true;
		d->cmdCv.notify_all();
	}
	else {
		// one still running is left alone; one that finished with no caller
		// waiting (the assembler given up on a detach) is over
		if (d->cmdRunning && !d->cmdDone)
			return A52DBG_CMD_OK;
		d->cmdRunning = true;
		d->cmdDone = false;
		std::string copy = line ? line : "";
		d->im->host.Post([d, copy] {
			std::vector<char> buf(copy.begin(), copy.end());
			buf.push_back(0);
			console_open(d);
			int rc = MONITOR_Execute(buf.data());
			fflush(mon_output);
			std::lock_guard<std::mutex> l(d->cmdMtx);
			d->cmdRc = rc;
			d->cmdDone = true;
			d->cmdCv.notify_all();
		}, A52Host::Kind::Any);
	}
	// until it finishes or asks for more (once it has taken this line)
	while (!(d->cmdDone || (d->waitingInput && !d->haveInput))) {
		if (d->im->host.Quitting() || !d->im->host.IsRunning()) {
			d->cmdRunning = false;
			return A52DBG_CMD_OK;
		}
		d->cmdCv.wait_for(lock, std::chrono::milliseconds(50));
	}
	bool done = d->cmdDone;
	int rc = d->cmdRc;
	if (done)
		d->cmdRunning = false;
	lock.unlock();

	std::string text;
	d->im->host.Call([&] { text = console_take(d); }, A52Host::Kind::Any);
	if (dst && dstsz > 0)
		snprintf(dst, (size_t)dstsz, "%s", text.c_str());
	if (!done)
		return A52DBG_CMD_NEEDS_INPUT;
	if (rc == TRUE) {
		// G, R, O, CONT: armed by the monitor
		go(d, Intent::Monitor, [d] {
			std::vector<MONITOR_breakpoint_cond> rows;
			if (d->attached.load())
				rows = encode_all(d->bps);
			int n = (int)std::min<size_t>(rows.size(), MONITOR_BREAKPOINT_TABLE_MAX);
			for (int i = 0; i < n; i++)
				MONITOR_breakpoint_table[i] = rows[i];
			MONITOR_breakpoint_table_size = n;
		});
		return A52DBG_CMD_RESUMED;
	}
	bump(d);
	return A52DBG_CMD_OK;
}

int a52debug_completions(a52debug* d, const char* prefix, char* dst, int dstsz)
{
	static const char* const cmds[] = {
		"CONT", "SHOW", "STACK", "SET", "SETPC", "SETA", "SETX", "SETY", "SETS", "SETN", "SETV",
		"SETD", "SETI", "SETZ", "SETC", "BBRK", "BPC", "HISTORY", "JUMPS", "G", "R", "O", "BLINE",
		"BREAK", "YBREAK", "D", "M", "F", "S", "C", "SUM", "LOOP", "A", "DLIST", "LABELS", "ANTIC",
		"GTIA", "POKEY", "ROM", "RAM", "HARDWARE", "READ", "WRITE", "SSTR", "SSCR",
		"FP", "MFP", "CFP", "HELP", nullptr};
	(void)d;
	std::string out, p = prefix ? prefix : "";
	for (auto& c : p)
		c = (char)toupper((unsigned char)c);
	for (int i = 0; cmds[i]; i++)
		if (!strncmp(cmds[i], p.c_str(), p.size()))
			out += std::string(out.empty() ? "" : " ") + cmds[i];
	return snprintf(dst, (size_t)dstsz, "%s", out.c_str());
}

// ---- the CPU ---------------------------------------------------------------------

void a52debug_cpu_get(a52debug* d, a52debug_cpu* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o] {
		CPU_GetStatus();
		o->pc = CPU_regPC;
		o->sp = CPU_regS;
		o->a = CPU_regA;
		o->x = CPU_regX;
		o->y = CPU_regY;
		o->p = CPU_regP;
		o->n = !!(CPU_regP & CPU_N_FLAG);
		o->v = !!(CPU_regP & CPU_V_FLAG);
		o->d = !!(CPU_regP & CPU_D_FLAG);
		o->i = !!(CPU_regP & CPU_I_FLAG);
		o->z = !!(CPU_regP & CPU_Z_FLAG);
		o->c = !!(CPU_regP & CPU_C_FLAG);
		o->scanline = ANTIC_ypos;
		o->cycle = ANTIC_xpos;
		o->frame = (uint32_t)Atari800_nframes;
	}, A52Host::Kind::Any);
}

void a52debug_cpu_set(a52debug* d, int reg, int value)
{
	d->im->host.Call([reg, value] {
		CPU_GetStatus();
		auto flag = [&](UBYTE f) {
			CPU_regP = (UBYTE)(value ? (CPU_regP | f) : (CPU_regP & ~f));
		};
		switch (reg) {
		case A52_REG_PC: CPU_regPC = (UWORD)value; break;
		case A52_REG_SP: CPU_regS = (UBYTE)value; break;
		case A52_REG_A: CPU_regA = (UBYTE)value; break;
		case A52_REG_X: CPU_regX = (UBYTE)value; break;
		case A52_REG_Y: CPU_regY = (UBYTE)value; break;
		case A52_REG_P: CPU_regP = (UBYTE)(value | 0x30); break;
		case A52_FLAG_N: flag(CPU_N_FLAG); break;
		case A52_FLAG_V: flag(CPU_V_FLAG); break;
		case A52_FLAG_D: flag(CPU_D_FLAG); break;
		case A52_FLAG_I: flag(CPU_I_FLAG); break;
		case A52_FLAG_Z: flag(CPU_Z_FLAG); break;
		case A52_FLAG_C: flag(CPU_C_FLAG); break;
		default: break;
		}
		CPU_PutStatus();
	}, A52Host::Kind::Any);
	bump(d);
}

static void split_line(const char* text, a52debug_line* l)
{
	// "ADDR: b1 b2 b3  MNEMONIC operand          ;label\n"
	std::string t = text;
	if (!t.empty() && t.back() == '\n')
		t.pop_back();
	std::string bytes = t.size() > 6 ? t.substr(6, 9) : "";
	std::string rest = t.size() > 16 ? t.substr(16) : "";
	std::string comment;
	size_t semi = rest.find(';');
	if (semi != std::string::npos) {
		comment = rest.substr(semi + 1);
		rest = rest.substr(0, semi);
	}
	while (!rest.empty() && rest.back() == ' ')
		rest.pop_back();
	while (!bytes.empty() && bytes.back() == ' ')
		bytes.pop_back();
	snprintf(l->bytes, sizeof l->bytes, "%s", bytes.c_str());
	snprintf(l->disasm, sizeof l->disasm, "%s", rest.c_str());
	snprintf(l->comment, sizeof l->comment, "%s", comment.c_str());
}

int a52debug_disassemble(a52debug* d, uint16_t addr, a52debug_line* out, int max, int* pc_line)
{
	int n = 0;
	if (pc_line)
		*pc_line = -1;
	struct Range { uint16_t start, end; bool enabled; };
	std::vector<Range> bpRanges;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		for (auto& b : d->bps)
			if (b.type & A52DEBUG_BP_EXEC)
				bpRanges.push_back({b.start, b.end, b.enabled});
	}
	d->im->host.Call([&] {
		uint16_t pc = addr;
		for (n = 0; n < max; n++) {
			char buf[128];
			a52debug_line* l = &out[n];
			memset(l, 0, sizeof *l);
			l->address = pc;
			l->is_pc = pc == CPU_regPC;
			if (l->is_pc && pc_line)
				*pc_line = n;
			for (auto& r : bpRanges)
				if (pc >= r.start && pc <= r.end && l->has_breakpoint != 1)
					l->has_breakpoint = r.enabled ? 1 : 2;
			std::string lab = label_for(pc, 0);
			snprintf(l->label, sizeof l->label, "%s", lab.c_str());
			uint16_t next = MONITOR_DisassembleLine(pc, buf, sizeof buf);
			split_line(buf, l);
			pc = next;
		}
	}, A52Host::Kind::Any);
	return n;
}

int a52debug_row_address(a52debug* d, uint16_t addr, int rows)
{
	int result = addr;
	d->im->host.Call([&] {
		char buf[128];
		if (rows >= 0) {
			uint16_t pc = addr;
			for (int i = 0; i < rows; i++)
				pc = MONITOR_DisassembleLine(pc, buf, sizeof buf);
			result = pc;
			return;
		}
		// backwards: the start that decodes cleanly into addr
		for (int back = -rows * 3; back >= -rows; back--) {
			uint16_t pc = (uint16_t)(addr - back);
			int count = 0;
			while (pc != addr && count < back) {
				pc = MONITOR_DisassembleLine(pc, buf, sizeof buf);
				count++;
				if ((uint16_t)(addr - pc) > (uint16_t)back)
					break;
			}
			if (pc == addr && count >= -rows) {
				uint16_t start = (uint16_t)(addr - back);
				for (int i = 0; i < count + rows; i++)
					start = MONITOR_DisassembleLine(start, buf, sizeof buf);
				result = start;
				return;
			}
		}
		result = (uint16_t)(addr + rows);
	}, A52Host::Kind::Any);
	return result;
}

int a52debug_history_get(a52debug* d, a52debug_history* out, int max)
{
	int n = 0;
	d->im->host.Call([&] {
		int total = CPU_REMEMBER_PC_STEPS < max ? CPU_REMEMBER_PC_STEPS : max;
		for (int i = CPU_REMEMBER_PC_STEPS - total; i < CPU_REMEMBER_PC_STEPS; i++) {
			int k = (CPU_remember_PC_curpos + i) % CPU_REMEMBER_PC_STEPS;
			uint16_t pc = CPU_remember_PC[k];
			// the bytes as they were when it ran
			UBYTE saved[3];
			char buf[128];
			for (int j = 0; j < 3; j++) {
				saved[j] = MEMORY_dGetByte((UWORD)(pc + j));
				MEMORY_dPutByte((UWORD)(pc + j), CPU_remember_op[k][j]);
			}
			MONITOR_DisassembleLine(pc, buf, sizeof buf);
			for (int j = 0; j < 3; j++)
				MEMORY_dPutByte((UWORD)(pc + j), saved[j]);
			a52debug_history* h = &out[n++];
			h->pc = pc;
			h->scanline = CPU_remember_xpos[k] >> 8;
			h->cycle = CPU_remember_xpos[k] & 0xff;
			std::string t = buf;
			if (!t.empty() && t.back() == '\n')
				t.pop_back();
			snprintf(h->disasm, sizeof h->disasm, "%s", t.size() > 16 ? t.c_str() + 16 : t.c_str());
		}
	}, A52Host::Kind::Any);
	return n;
}

// ---- memory --------------------------------------------------------------------------

int a52debug_read(a52debug* d, uint16_t addr, uint8_t* dst, int n)
{
	d->im->host.Call([&] {
		for (int i = 0; i < n; i++)
			dst[i] = MEMORY_SafeGetByte((UWORD)(addr + i));
	}, A52Host::Kind::Any);
	return n;
}

void a52debug_write(a52debug* d, uint16_t addr, uint8_t value)
{
	d->im->host.Call([&] {
		if (addr >= 0x4000 && addr < 0xC000) {
			// the cartridge window: the view's own buffer (a CPU write would be a
			// cycle on the cart edge, with a hotspot's side effects), and the copy
			// ANTIC reads
			if (d->im->cart)
				a52_fujinet_view_poke(d->im->cart, (uint32_t)(addr - 0x4000), value);
			MEMORY_mem[addr] = value;
		}
		else
			MEMORY_PutByte(addr, value);
	}, A52Host::Kind::Any);
	bump(d);
}

int a52debug_label_address(a52debug* d, const char* label)
{
	int v = -1;
	d->im->host.Call([&] {
		v = MONITOR_LabelValue(label);
		for (const Label* e = kExtraLabels; v < 0 && e->name; e++)
			if (ci_equal(e->name, label))
				v = e->addr;
	}, A52Host::Kind::Any);
	return v;
}

int a52debug_address_label(a52debug* d, uint16_t addr, char* dst, int dstsz)
{
	std::string out;
	d->im->host.Call([&] { out = label_for(addr, 0); }, A52Host::Kind::Any);
	return snprintf(dst, (size_t)dstsz, "%s", out.c_str());
}

int a52debug_load_symbols(a52debug* d, const char* path, char* msg, int msgsz)
{
	// ca65/ld65 -Ln and VICE ("al C:1234 .name"), MADS .lab ("00 1234 NAME"),
	// atari800's own ("1234 NAME"): into the monitor with LABELS SET, beside
	// the 5200 hardware labels (a path with spaces would not survive LABELS ADD)
	FILE* in = fopen(path, "r");
	if (!in) {
		snprintf(msg, (size_t)msgsz, "Could not open it");
		return -1;
	}
	std::vector<std::pair<std::string, unsigned>> labels;
	char line[512];
	while (fgets(line, sizeof line, in)) {
		unsigned addr;
		char name[256], bank[16];
		if (sscanf(line, "al %*[^:]:%x .%255s", &addr, name) == 2
		    || sscanf(line, "al %x .%255s", &addr, name) == 2
		    || sscanf(line, "%15s %x %255s", bank, &addr, name) == 3
		    || sscanf(line, "%x %255s", &addr, name) == 2
		    || sscanf(line, "%255s = $%x", name, &addr) == 2) {
			bool ok = isalpha((unsigned char)name[0]) || strchr("_@?", name[0]);
			for (char* c = name; ok && *c; c++)
				ok = isalnum((unsigned char)*c) || strchr("_@?.", *c);
			if (ok)
				labels.push_back({name, addr & 0xffff});
		}
	}
	fclose(in);
	if (labels.empty()) {
		snprintf(msg, (size_t)msgsz, "No labels in it");
		return -1;
	}
	d->im->host.Call([&] {
		char cmd[300];
		console_open(d);
		snprintf(cmd, sizeof cmd, "LABELS BUILTIN");
		MONITOR_Execute(cmd);
		for (auto& l : labels) {
			snprintf(cmd, sizeof cmd, "LABELS SET %s %04X", l.first.c_str(), l.second);
			MONITOR_Execute(cmd);
		}
		fflush(mon_output);
	}, A52Host::Kind::Any);
	snprintf(msg, (size_t)msgsz, "%d labels", (int)labels.size());
	bump(d);
	return (int)labels.size();
}

// ---- ANTIC and the display list ----------------------------------------------------------

void a52debug_antic_get(a52debug* d, a52debug_antic* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o] {
		o->dmactl = ANTIC_DMACTL;
		o->chactl = ANTIC_CHACTL;
		o->hscrol = ANTIC_HSCROL;
		o->vscrol = ANTIC_VSCROL;
		o->pmbase = ANTIC_PMBASE;
		o->chbase = ANTIC_CHBASE;
		o->nmien = ANTIC_NMIEN;
		o->nmist = ANTIC_NMIST;
		o->dlist = dlist_start();
		o->scanline = ANTIC_ypos;
		o->cycle = ANTIC_xpos;
		o->vcount = ANTIC_GetByte(0x0b, TRUE);
		o->playfield_width = ANTIC_DMACTL & 3;
		o->dl_dma = !!(ANTIC_DMACTL & 0x20);
		o->pm_dma = !!(ANTIC_DMACTL & 0x08);
		o->missile_dma = !!(ANTIC_DMACTL & 0x04);
		o->pm_single_line = !!(ANTIC_DMACTL & 0x10);
	}, A52Host::Kind::Any);
}

int a52debug_dlist_text(a52debug* d, char* dst, int dstsz)
{
	std::string out;
	d->im->host.Call([&] {
		UWORD pc = dlist_start();
		std::string last;
		int repeat = 0;
		UWORD lastAddr = 0;
		auto flush = [&]() {
			if (last.empty())
				return;
			out += fmt("$%04X  ", lastAddr);
			if (repeat > 1)
				out += fmt("%dx ", repeat);
			out += last + "\n";
			last.clear();
			repeat = 0;
		};
		for (int count = 0; count < 300; count++) {
			UWORD at = pc;
			UBYTE ir = MEMORY_SafeGetByte(pc);
			pc = (UWORD)((pc & 0xfc00) | ((pc + 1) & 0x03ff));   // ANTIC's 1K wrap
			std::string text, flags;
			if (ir & 0x80)
				flags += " DLI";
			UBYTE mode = ir & 0x0f;
			if (mode == 0)
				text = fmt("BLANK %d", ((ir >> 4) & 7) + 1);
			else if (mode == 1) {
				UWORD target = MEMORY_SafeGetByte(pc) | (MEMORY_SafeGetByte((UWORD)(pc + 1)) << 8);
				text = fmt("%s $%04X", (ir & 0x40) ? "JVB" : "JMP", target);
				flush();
				out += fmt("$%04X  %s%s\n", at, text.c_str(), flags.c_str());
				if (ir & 0x40)
					break;
				pc = target;
				continue;
			}
			else {
				if (ir & 0x40) {
					UWORD lms = MEMORY_SafeGetByte(pc) | (MEMORY_SafeGetByte((UWORD)(pc + 1)) << 8);
					pc = (UWORD)(pc + 2);
					flags += fmt(" LMS $%04X", lms);
				}
				if (ir & 0x20)
					flags += " VSCROL";
				if (ir & 0x10)
					flags += " HSCROL";
				text = fmt("MODE %X", mode);
			}
			std::string full = text + flags;
			if (full == last && full.find("LMS") == std::string::npos)
				repeat++;
			else {
				flush();
				last = full;
				lastAddr = at;
				repeat = 1;
			}
		}
		flush();
	}, A52Host::Kind::Any);
	return snprintf(dst, (size_t)dstsz, "%s", out.c_str());
}

// ---- GTIA ------------------------------------------------------------------------------------

void a52debug_gtia_get(a52debug* d, a52debug_gtia* o)
{
	memset(o, 0, sizeof *o);
	d->im->host.Call([o] {
		const UBYTE hp[4] = {GTIA_HPOSP0, GTIA_HPOSP1, GTIA_HPOSP2, GTIA_HPOSP3};
		const UBYTE hm[4] = {GTIA_HPOSM0, GTIA_HPOSM1, GTIA_HPOSM2, GTIA_HPOSM3};
		const UBYTE sp[4] = {GTIA_SIZEP0, GTIA_SIZEP1, GTIA_SIZEP2, GTIA_SIZEP3};
		const UBYTE gp[4] = {GTIA_GRAFP0, GTIA_GRAFP1, GTIA_GRAFP2, GTIA_GRAFP3};
		const UBYTE cp[4] = {GTIA_COLPM0, GTIA_COLPM1, GTIA_COLPM2, GTIA_COLPM3};
		const UBYTE cf[4] = {GTIA_COLPF0, GTIA_COLPF1, GTIA_COLPF2, GTIA_COLPF3};
		for (int i = 0; i < 4; i++) {
			o->hposp[i] = hp[i];
			o->hposm[i] = hm[i];
			o->sizep[i] = sp[i];
			o->grafp[i] = gp[i];
			o->colpm[i] = cp[i];
			o->colpf[i] = cf[i];
			o->mpf[i] = GTIA_GetByte((UWORD)(0x00 + i), TRUE);
			o->ppf[i] = GTIA_GetByte((UWORD)(0x04 + i), TRUE);
			o->mpl[i] = GTIA_GetByte((UWORD)(0x08 + i), TRUE);
			o->ppl[i] = GTIA_GetByte((UWORD)(0x0c + i), TRUE);
			o->trig[i] = GTIA_GetByte((UWORD)(0x10 + i), TRUE);
		}
		o->sizem = GTIA_SIZEM;
		o->grafm = GTIA_GRAFM;
		o->colbk = GTIA_COLBK;
		o->prior = GTIA_PRIOR;
		o->vdelay = GTIA_VDELAY;
		o->gractl = GTIA_GRACTL;
		o->consol = GTIA_GetByte(0x1f, TRUE);
	}, A52Host::Kind::Any);
}

uint32_t a52debug_color(a52debug* d, uint8_t index)
{
	uint32_t c = 0;
	d->im->host.Call([&] { c = a8video_colour(index); }, A52Host::Kind::Any);
	return c & 0xffffff;
}

int a52debug_palette_image(a52debug* d, uint32_t* dst)
{
	uint32_t pal[256];
	d->im->host.Call([&] {
		for (int i = 0; i < 256; i++)
			pal[i] = a8video_colour(i);
	}, A52Host::Kind::Any);
	// 16 hues down, 16 lumas across; each colour 16 x 4 pixels
	for (int y = 0; y < A52DEBUG_PALETTE_HEIGHT; y++)
		for (int x = 0; x < A52DEBUG_PALETTE_WIDTH; x++)
			dst[y * A52DEBUG_PALETTE_WIDTH + x] = pal[((y / 4) << 4) | (x / 16)] | 0xff000000u;
	return 1;
}

// ---- POKEY ------------------------------------------------------------------------

void a52debug_pokey_get(a52debug* d, int chip, a52debug_pokey* o)
{
	memset(o, 0, sizeof *o);
	if (chip)
		return;   // the 5200 has one POKEY
	d->im->host.Call([o] {
		const int c = 0;
		for (int i = 0; i < 4; i++) {
			o->audf[i] = POKEY_AUDF[c * 4 + i];
			o->audc[i] = POKEY_AUDC[c * 4 + i];
		}
		o->audctl = POKEY_AUDCTL[c];
		o->second = 0;
		o->kbcode = POKEY_KBCODE;
		o->skstat = POKEY_SKSTAT;
		o->skctl = POKEY_SKCTL;
		o->irqen = POKEY_IRQEN;
		o->irqst = POKEY_IRQST;
		o->random = POKEY_GetByte(0x0a, TRUE);
		for (int i = 0; i < 8; i++)
			o->pot[i] = POKEY_POT_input[i];
	}, A52Host::Kind::Any);
}

// ---- the memory map and the FujiNet cartridge ----------------------------------

static void cart_status(a52debug* d, a52_cart_status_t* st)
{
	memset(st, 0, sizeof *st);
	if (d->im->cart)
		a52_fujinet_status(d->im->cart, st);
}

void a52debug_map_get(a52debug* d, a52debug_map* o)
{
	memset(o, 0, sizeof *o);
	a52session_bios_name(d->s, o->bios, sizeof o->bios);
	d->im->host.Call([o] { o->tv_pal = Atari800_tv_mode == Atari800_TV_PAL; }, A52Host::Kind::Any);
	a52_cart_status_t st;
	cart_status(d, &st);
	o->cart_powered = st.powered;
	o->mapper = st.mapper;
	o->mapper_name = a52_fujinet_kind_name(st.mapper);
	o->nbanks = st.nbanks;
	o->bank[0] = st.bank[0];
	o->bank[1] = st.bank[1];
	o->mailbox = st.mailbox;
	o->link_up = st.link_up;
	o->link_off = st.link_off;
	o->claim = st.claim;
	o->booted_game = st.booted_game;
	o->armed = st.armed;
	o->staged = st.staged;
	o->staged_claim = st.staged_claim;
	o->staged_config = st.staged_config;
	o->staged_mapper = st.staged_mapper;
	o->live_size = st.live_size;
	o->live_crc = st.live_crc;
	o->staged_size = st.staged_size;
	o->staged_crc = st.staged_crc;
	o->swaps = (int)st.swaps;
	o->boot_state = st.boot_state;
	o->boot_pct = st.boot_pct;
	o->boot_err = st.boot_err;
	o->ackseq = st.ackseq;
	o->status = st.status;
	o->last_error = st.err;
	o->reply_cmd = st.reply_cmd;
	o->rxlen = st.rxlen;
	o->queue_depth = st.queue;
	if (d->im->cart)
		a52_fujinet_arena(d->im->cart, o->arena);
	else
		memset(o->arena, 0xff, sizeof o->arena);
	if (!st.powered)
		snprintf(o->cart_name, sizeof o->cart_name, "No cartridge power");
	else if (!st.booted_game)
		snprintf(o->cart_name, sizeof o->cart_name, "FujiNet CONFIG");
	else if (st.mailbox)
		snprintf(o->cart_name, sizeof o->cart_name, "FujiNet app, %s", o->mapper_name);
	else {
		if (st.mapper == 3 && st.nbanks)
			snprintf(o->cart_name, sizeof o->cart_name, "Game, %s %d KB", o->mapper_name, st.nbanks * 32);
		else
			snprintf(o->cart_name, sizeof o->cart_name, "Game, %s", o->mapper_name);
	}
}

int a52debug_cart_info(a52debug* d, char* dst, int dstsz)
{
	a52debug_map m;
	int n;
	a52debug_map_get(d, &m);
	if (!m.cart_powered)
		return snprintf(dst, (size_t)dstsz, "FujiNet cartridge: off");
	const char* what = !m.booted_game ? "CONFIG" : m.mailbox ? "app (mailbox live)" : "game image";
	const char* link = m.link_off ? "off" : m.link_up ? "up" : "down";
	if (m.live_size)
		n = snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, %s, link %s, %u bytes", what, m.mapper_name,
		             link, m.live_size);
	else
		n = snprintf(dst, (size_t)dstsz, "FujiNet cartridge: %s, link %s", what, link);
	return n < dstsz ? n : dstsz - 1;
}

// ---- breakpoints -------------------------------------------------------------------------

static void rearm(a52debug* d)
{
	d->im->host.Call([d] { arm(d); }, A52Host::Kind::Any);
	bump(d);
}

int a52debug_breakpoint_add(a52debug* d, int type, uint16_t start, uint16_t end,
                             const char* condition, char* msg, int msgsz)
{
	Bp b;
	std::vector<Cond> conds;
	std::string err;
	if (!type)
		type = A52DEBUG_BP_EXEC;
	if (end < start)
		std::swap(start, end);
	if (condition && !parse_conditions(condition, conds, err)) {
		if (msg && msgsz > 0)
			snprintf(msg, (size_t)msgsz, "%s", err.c_str());
		return -1;
	}
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		b.id = type == A52DEBUG_BP_EXEC ? d->nextBp++ : d->nextWp++;
		b.type = type;
		b.start = start;
		b.end = end;
		b.enabled = true;
		b.condition = condition ? condition : "";
		std::vector<Bp> trial = d->bps;
		trial.push_back(b);
		if (encode_all(trial).size() > MONITOR_BREAKPOINT_TABLE_MAX) {
			if (msg && msgsz > 0)
				snprintf(msg, (size_t)msgsz, "atari800 holds %d breakpoint conditions; this one does not fit",
				         MONITOR_BREAKPOINT_TABLE_MAX);
			return -1;
		}
		d->bps.push_back(b);
	}
	rearm(d);
	return b.id;
}

int a52debug_breakpoint_toggle(a52debug* d, uint16_t addr)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		for (size_t i = 0; i < d->bps.size(); i++)
			if (d->bps[i].type == A52DEBUG_BP_EXEC && d->bps[i].start == addr && d->bps[i].end == addr) {
				d->bps.erase(d->bps.begin() + (long)i);
				goto removed;
			}
	}
	return a52debug_breakpoint_add(d, A52DEBUG_BP_EXEC, addr, addr, "", nullptr, 0) > 0 ? 1 : -1;
removed:
	rearm(d);
	return 0;
}

int a52debug_breakpoint_check(a52debug* d, uint16_t addr)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	for (auto& b : d->bps)
		if ((b.type & A52DEBUG_BP_EXEC) && addr >= b.start && addr <= b.end)
			return b.enabled ? 1 : 2;
	return 0;
}

void a52debug_breakpoint_remove(a52debug* d, int id)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		d->bps.erase(std::remove_if(d->bps.begin(), d->bps.end(), [id](const Bp& b) { return b.id == id; }),
		             d->bps.end());
	}
	rearm(d);
}

void a52debug_breakpoint_enable(a52debug* d, int id, int enabled)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		for (auto& b : d->bps)
			if (b.id == id)
				b.enabled = enabled != 0;
	}
	rearm(d);
}

int a52debug_breakpoint_list(a52debug* d, a52debug_breakpoint* out, int max)
{
	std::lock_guard<std::mutex> lock(d->mtx);
	int n = 0;
	for (auto& b : d->bps) {
		if (n >= max)
			break;
		out[n].id = b.id;
		out[n].type = b.type;
		out[n].start = b.start;
		out[n].end = b.end;
		out[n].enabled = b.enabled;
		snprintf(out[n].condition, sizeof out[n].condition, "%s", b.condition.c_str());
		n++;
	}
	return n;
}

void a52debug_breakpoint_clear(a52debug* d)
{
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		d->bps.clear();
	}
	rearm(d);
}

int a52debug_breakpoint_budget(a52debug* d, int* max)
{
	if (max)
		*max = MONITOR_BREAKPOINT_TABLE_MAX;
	std::lock_guard<std::mutex> lock(d->mtx);
	return (int)encode_all(d->bps).size();
}

// ---- the FujiNet trace ---------------------------------------------------------------------

int a52debug_fujinet_trace(a52debug* d, a52_fuji_packet* out, int max)
{
	std::lock_guard<std::mutex> lock(d->fujiMtx);
	return fujitrace_read(&d->fuji, out, max);
}

void a52debug_fujinet_trace_clear(a52debug* d)
{
	std::lock_guard<std::mutex> lock(d->fujiMtx);
	fujitrace_clear(&d->fuji);
}

int a52debug_fujinet_state(a52debug* d, char* dst, int dstsz)
{
	a52debug_map c;
	int n;
	if (!d->s->running)
		return snprintf(dst, (size_t)dstsz, "The 5200 is off\n");
	a52debug_map_get(d, &c);
	if (!c.cart_powered)
		return snprintf(dst, (size_t)dstsz, "The cartridge has no power.\n");
	const char* mode = c.link_off ? "off (FujiNet is switched off)"
	                              : c.link_up ? "up" : "down (the cartridge redials before its next transaction)";
	std::string bank;
	if (c.mapper == 2)
		bank = fmt("banks $%d / $%d", c.bank[0], c.bank[1]);
	else if (c.mapper == 3)
		bank = fmt("bank %d of %d", c.bank[0], c.nbanks);
	else
		bank = "no banking";
	n = snprintf(dst, (size_t)dstsz,
	    "Link         %s\n"
	    "Endpoint     %s\n"
	    "Image        %s, %s (%s), %u bytes, CRC %08X\n"
	    "Mailbox      %s (claim %s)\n"
	    "\n"
	    "Mailbox registers\n"
	    "  ACKSEQ     $%02X\n"
	    "  STATUS     $%02X  (link %d, busy %d)\n"
	    "  last error %u\n"
	    "  reply cmd  $%02X\n"
	    "  RXLEN      %u\n"
	    "  queue      %u hotspot reads waiting\n"
	    "\n"
	    "Boot         state %d  %d%%\n"
	    "             error %d\n"
	    "Staged       %s%s%s  %u bytes, CRC %08X\n"
	    "Swap         %s\n"
	    "Swaps        %d\n",
	    mode, d->s->boip_endpoint[0] ? d->s->boip_endpoint : "-",
	    c.cart_name, c.mapper_name, bank.c_str(), c.live_size, c.live_crc,
	    c.mailbox ? "live ($B000-$B7FF is the arena)" : "off", c.claim ? "FUJI" : "none",
	    c.ackseq, c.status, c.status & 1, (c.status >> 1) & 1, c.last_error, c.reply_cmd, c.rxlen,
	    c.queue_depth, c.boot_state, c.boot_pct, c.boot_err,
	    c.staged ? (c.staged_config ? "CONFIG" : c.staged_claim ? "an app" : "a game") : "nothing",
	    c.staged ? ", " : "", c.staged ? a52_fujinet_kind_name(c.staged_mapper) : "",
	    c.staged_size, c.staged_crc,
	    c.armed ? "armed: the read of $B5F2 swaps it in" : c.staged ? "staged, not armed" : "idle",
	    c.swaps);
	return n < dstsz ? n : dstsz - 1;
}

int a52debug_save(a52debug* d, const char* kind, const char* path, char* msg, int msgsz)
{
	FILE* f = fopen(path, "wb");
	if (!f) {
		snprintf(msg, (size_t)msgsz, "Could not write %s", path);
		return -1;
	}
	if (!strcmp(kind, "ram")) {
		std::vector<uint8_t> mem(65536);
		a52debug_read(d, 0, mem.data(), 65536);
		fwrite(mem.data(), 1, mem.size(), f);
		snprintf(msg, (size_t)msgsz, "64K saved");
	}
	else if (!strcmp(kind, "cart")) {
		std::vector<uint8_t> mem(0x8000);
		d->im->host.Call([&] {
			for (uint32_t i = 0; i < 0x8000; i++)
				mem[i] = d->im->cart ? a52_fujinet_view_byte(d->im->cart, i) : 0xff;
		}, A52Host::Kind::Any);
		fwrite(mem.data(), 1, mem.size(), f);
		snprintf(msg, (size_t)msgsz, "cartridge window saved (32K)");
	}
	else if (!strcmp(kind, "arena")) {
		a52debug_map m;
		a52debug_map_get(d, &m);
		fwrite(m.arena, 1, sizeof m.arena, f);
		snprintf(msg, (size_t)msgsz, "arena saved (2K)");
	}
	else if (!strcmp(kind, "screen")) {
		std::vector<uint32_t> fb((size_t)A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT);
		int w = 0, h = 0, par = 0;
		uint64_t serial = 0;
		if (!a52session_copy_frame(d->s, fb.data(), &w, &h, &par, &serial) || w <= 0 || h <= 0) {
			fclose(f);
			snprintf(msg, (size_t)msgsz, "No picture yet");
			return -1;
		}
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int i = 0; i < w * h; i++) {
			uint8_t px[3] = {(uint8_t)(fb[(size_t)i] >> 16), (uint8_t)(fb[(size_t)i] >> 8), (uint8_t)fb[(size_t)i]};
			fwrite(px, 1, 3, f);
		}
		snprintf(msg, (size_t)msgsz, "%dx%d picture saved", w, h);
	}
	else {
		a52debug_cpu cpu;
		a52debug_cpu_get(d, &cpu);
		std::vector<a52debug_line> lines(256);
		int n = a52debug_disassemble(d, (uint16_t)cpu.pc, lines.data(), 256, nullptr);
		for (int i = 0; i < n; i++)
			fprintf(f, "%04X  %-9s %-24s %s%s%s\n", lines[i].address, lines[i].bytes, lines[i].disasm,
			        lines[i].label[0] ? lines[i].label : "", lines[i].comment[0] ? " ;" : "",
			        lines[i].comment);
		snprintf(msg, (size_t)msgsz, "%d lines saved", n);
	}
	fclose(f);
	return 0;
}
