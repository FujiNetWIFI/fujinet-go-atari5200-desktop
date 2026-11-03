/*
 * session.cpp -- a52session: the emulator (core/atari800/A52Host, atari800
 * forced into 5200 mode) and the FujiNet cartridge (core/cart) under the
 * settings, input, media and FujiNet the frontends drive.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "A52Host.h"
#include "fujinet_cart.h"
#include "fujitcp_host.h"
#include "session_impl.h"
#include "session_internal.h"

extern "C" {
#include "config.h"
#include "akey.h"
#include "atari.h"
#include "cfg.h"
#include "colours.h"
#include "colours_external.h"
#include "colours_ntsc.h"
#include "colours_pal.h"
#include "cpu.h"
#include "input.h"
#include "log.h"
#include "memory.h"
#include "monitor.h"
#include "pokeysnd.h"
#include "sound.h"
#include "a8host.h"
#include "a8video.h"
}

namespace {
SessionImpl* gImpl = nullptr;   // the cart hooks' context (one session at a time)
}

SessionImpl* Impl(a52session* s)
{
	return static_cast<SessionImpl*>(s->impl);
}

void session_set_error(struct a52session* s, const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->last_error, sizeof s->last_error, fmt, ap);
	va_end(ap);
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

a52session* a52session_new(const a52session_paths* paths)
{
	a52session* s = (a52session*)calloc(1, sizeof *s);
	if (!s)
		return nullptr;
	pthread_mutex_init(&s->sysact_mtx, nullptr);
	pthread_mutex_init(&s->pad_event_mtx, nullptr);
	if (paths_init(s, paths ? paths->config_dir : nullptr, paths ? paths->data_dir : nullptr) != 0) {
		pthread_mutex_destroy(&s->sysact_mtx);
		pthread_mutex_destroy(&s->pad_event_mtx);
		free(s);
		return nullptr;
	}
	if (paths && paths->fujinet_lib) {
		snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", paths->fujinet_lib);
		s->fujinet_disabled = !*paths->fujinet_lib;
	}
	if (paths && paths->fujinet_runtime_src)
		snprintf(s->fujinet_runtime_src, sizeof s->fujinet_runtime_src, "%s",
		         paths->fujinet_runtime_src);
	settings_init(s);
	s->impl = new SessionImpl(s);
	roms_provision_embedded(s);
	return s;
}

void a52session_free(a52session* s)
{
	if (!s)
		return;
	a52session_stop(s);
	session_debug_free(s);
	SessionImpl* im = Impl(s);
	if (im->coreUp) {
		// no emulation thread now: the core is ours to tear down
		a8host_set_cart(nullptr);
		Atari800_Exit(FALSE);
		im->coreUp = false;
	}
	if (im->cart) {
		a52_fujinet_destroy(im->cart);
		im->cart = nullptr;
	}
	if (gImpl == im)
		gImpl = nullptr;
	delete im;
	a52session_settings_flush(s);
	settings_free_all(s);
	pthread_mutex_destroy(&s->sysact_mtx);
	pthread_mutex_destroy(&s->pad_event_mtx);
	free(s);
}

void a52session_default_opts(a52session* s, a52session_start_opts* o)
{
	o->enable_fujinet = a52session_get_int(s, "enable_fujinet", 1);
	o->external_fujinet = a52session_get_int(s, "fujinet_external", 0);
	o->fujinet_addr = a52session_get_str(s, "fujinet_external_addr", NULL);
	o->enable_audio = a52session_get_int(s, "enable_audio", 1);
	o->enable_gamepad = a52session_get_int(s, "enable_gamepad", 1);
	o->cart_path = a52session_get_str(s, "cart", "");
	o->sync_cart = 0;
}

int a52session_is_running(const a52session* s)
{
	return s->running;
}

const char* a52session_last_error(const a52session* s)
{
	return s->last_error;
}

// ---- on the emulation thread -------------------------------------------------

static void apply_option(void*, const char* key, const char* val)
{
	char k[128], v[1100];
	snprintf(k, sizeof k, "%s", key);
	snprintf(v, sizeof v, "%s", val);
	CFG_ApplyOption(k, v);
}

// The palette: a preset plus the user's adjustments (the GTIA is the same
// NTSC or PAL, so both tables get the same setup).
void SessionImpl::ApplyVideo()
{
	static const Colours_setup_t presets[] = {
		{0.0, 0.0, 0.0, 0.0, 2.35, 0.0, 16, 235},
		{0.0, 0.0, 0.08, -0.08, 2.35, 0.0, 16, 235},
		{0.0, 0.26, 0.72, -0.16, 2.00, 0.0, 16, 235},
	};
	auto adj = [&](const char* key) { return a52session_get_int(s, key, 0) / 100.0; };
	auto clamp = [](double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; };
	int p = a52session_get_int(s, "palette_preset", 0);
	if (p < 0 || p > 2)
		p = 0;
	Colours_setup_t c = presets[p];
	c.hue = clamp(c.hue + adj("hue"), COLOURS_HUE_MIN, COLOURS_HUE_MAX);
	c.saturation = clamp(c.saturation + adj("saturation"), COLOURS_SATURATION_MIN, COLOURS_SATURATION_MAX);
	c.contrast = clamp(c.contrast + adj("contrast"), COLOURS_CONTRAST_MIN, COLOURS_CONTRAST_MAX);
	c.brightness = clamp(c.brightness + adj("brightness"), COLOURS_BRIGHTNESS_MIN, COLOURS_BRIGHTNESS_MAX);
	c.gamma = clamp(c.gamma + adj("gamma"), COLOURS_GAMMA_MIN, COLOURS_GAMMA_MAX);
	c.color_delay = COLOURS_NTSC_setup.color_delay;
	COLOURS_NTSC_setup = c;
	c.color_delay = COLOURS_PAL_setup.color_delay;
	COLOURS_PAL_setup = c;

	const char* pal = a52session_get_str(s, "palette_file", "");
	if (*pal) {
		COLOURS_EXTERNAL_ReadFilename(&COLOURS_NTSC_external, (char*)pal);
		COLOURS_EXTERNAL_ReadFilename(&COLOURS_PAL_external, (char*)pal);
	}
	else {
		COLOURS_EXTERNAL_Remove(&COLOURS_NTSC_external);
		COLOURS_EXTERNAL_Remove(&COLOURS_PAL_external);
	}
	Colours_Update();
	a8video_invalidate();
	area.store(a52session_get_int(s, "area", 1));
}

// Settings that apply without a restart.
void SessionImpl::ApplyLive()
{
	prefs_emit_live(s, apply_option, nullptr);
	ApplyVideo();
	int af = a52session_get_int(s, "autofire", 0);
	for (int p = 0; p < 4; p++)
		INPUT_joy_autofire[p] = af == 1 ? INPUT_AUTOFIRE_FIRE : af == 2 ? INPUT_AUTOFIRE_CONT
		                                                               : INPUT_AUTOFIRE_OFF;
	analog.store(a52session_get_int(s, "analog_stick", 1) != 0);
	host.SetVolume(a52session_get_int(s, "volume", 80));
}

// ---- the cartridge's glue into atari800 (patch 0006) ---------------------------

// ANTIC reads cart ROM straight from MEMORY_mem: keep it equal to what the
// cartridge serves whenever its view changes (a swap, a bank switch).
void SessionImpl::RefreshView()
{
	const unsigned v = a52_fujinet_view_serial(cart);
	if (v == viewSerial.load(std::memory_order_relaxed))
		return;
	viewSerial.store(v, std::memory_order_relaxed);
	a52_fujinet_copy_window(cart, MEMORY_mem + 0x4000);
}

static unsigned char CartGet(void*, unsigned short addr, int noSideEffects)
{
	SessionImpl* im = gImpl;
	const unsigned char b = a52_fujinet_read(im->cart, addr, noSideEffects);
	if (!noSideEffects)
		im->RefreshView();
	return b;
}

static void CartPut(void*, unsigned short addr, unsigned char value)
{
	SessionImpl* im = gImpl;
	a52_fujinet_write(im->cart, addr, value);
	im->RefreshView();
}

// The hand-over stub, JMP ($FFFC) at $B5F0, is executed from the cartridge:
// the read of its last byte swaps the armed image in (before the BIOS's
// reset code is fetched).
static void CartJmpInd(void*, unsigned short pc)
{
	if (pc != 0xB5F0)
		return;
	SessionImpl* im = gImpl;
	(void)a52_fujinet_read(im->cart, 0xB5F0, 0);
	(void)a52_fujinet_read(im->cart, 0xB5F1, 0);
	(void)a52_fujinet_read(im->cart, 0xB5F2, 0);
	im->RefreshView();
}

void SessionImpl::AttachCart()
{
	a8host_cart hooks = {};
	hooks.get = CartGet;
	hooks.put = CartPut;
	hooks.jmp_ind = CartJmpInd;
	MEMORY_SetHARDWARE(0x4000, 0xbfff);
	viewSerial.store(a52_fujinet_view_serial(cart) + 1);   // force the copy
	RefreshView();
	a8host_set_cart(&hooks);
}

// Power on: the cartridge, then the console (a real power cycle clears the RAM
// and restarts both; a5200 has no reset line to the cart).
void SessionImpl::ColdStart(bool initMachine)
{
	std::vector<uint8_t> image;
	std::string boip;
	bool sync;
	{
		std::lock_guard<std::mutex> lock(cartMtx);
		image = cartImage;
		boip = cartBoip;
		sync = cartSync;
	}
	char why[256] = "";
	if (a52_fujinet_power_on(cart, image.empty() ? nullptr : image.data(),
	                         (uint32_t)image.size(), boip.c_str(), sync, why, sizeof why) != 0) {
		Notice(std::string("The cartridge cannot run that image: ") + why);
		{
			std::lock_guard<std::mutex> lock(cartMtx);
			cartImage.clear();
		}
		s->cart_path[0] = 0;
		a52session_set_str(s, "cart", "");
		a52_fujinet_power_on(cart, nullptr, 0, boip.c_str(), sync, why, sizeof why);
	}
	CPU_cim_encountered = FALSE;   // a crash left behind with no debugger
	if (initMachine)
		Atari800_InitialiseMachine();
	Atari800_Coldstart();
	AttachCart();
}

bool SessionImpl::CoreInit(std::string& err)
{
	char arg0[] = "fujinet-go-atari5200";
	char arg1[] = "-5200";
	char* argv[3] = {arg0, arg1, nullptr};
	int argc = 2;
	Atari800_host_paced = TRUE;
	CFG_save_on_exit = FALSE;
	Sound_desired.freq = A8HOST_AUDIO_RATE;
	Sound_desired.sample_size = 2;
	Sound_desired.channels = 1;
	Sound_latency = 0;
	logText.clear();
	if (!cart) {
		cart = a52_fujinet_create();
		if (!cart) {
			err = "the FujiNet cartridge could not be created";
			return false;
		}
	}
	gImpl = this;
	for (int i = 0; i < 8; i++)
		a8host_pot[i] = 114;
	if (!Atari800_Initialise(&argc, argv)) {
		err = "atari800 did not start";
		if (Log_buffer[0]) {
			err += ": ";
			err += Log_buffer;
		}
		Log_buffer[0] = 0;
		return false;
	}
	coreUp = true;
	{
		std::lock_guard<std::mutex> lock(logMtx);
		const int slot = prefs_bios_slot(s);
		biosName = slot >= 0 ? a52session_rom_slot(slot)->name : "Built-in Altirra 5200 OS";
	}
	ApplyLive();
	ColdStart(false);
	return true;
}

// atari800 again from the top: BIOS, TV system and POKEY changes.
bool SessionImpl::CoreReinit(std::string& err)
{
	if (coreUp) {
		a8host_set_cart(nullptr);
		Atari800_Exit(FALSE);
		coreUp = false;
	}
	return CoreInit(err);
}

// Input staged from any thread, into the core before each frame.
void SessionImpl::BeforeFrame()
{
	static const int akey[A52_KEY_COUNT] = {
		AKEY_5200_0, AKEY_5200_1, AKEY_5200_2, AKEY_5200_3, AKEY_5200_4, AKEY_5200_5,
		AKEY_5200_6, AKEY_5200_7, AKEY_5200_8, AKEY_5200_9, AKEY_5200_ASTERISK,
		AKEY_5200_HASH, AKEY_5200_START, AKEY_5200_PAUSE, AKEY_5200_RESET};
	unsigned fire2 = 0;
	for (int p = 0; p < A52_PORTS; p++) {
		const unsigned b = joy[p].load() | joyPad[p].load();
		unsigned char stick = INPUT_STICK_CENTRE;
		if (b & (1u << A52_ACT_UP))
			stick &= ~1;
		if (b & (1u << A52_ACT_DOWN))
			stick &= ~2;
		if (b & (1u << A52_ACT_LEFT))
			stick &= ~4;
		if (b & (1u << A52_ACT_RIGHT))
			stick &= ~8;
		a8host_stick[p] = stick;
		a8host_trig[p] = (b & (1u << A52_ACT_FIRE)) ? 0 : 1;
		fire2 |= b & (1u << A52_ACT_FIRE2);
		// the analog pots: the strongest of the pad and the pointer; centred
		// (114) hands the decision to the digital stick above
		int x = padX[p].load(), y = padY[p].load();
		const int px = ptrX[p].load(), py = ptrY[p].load();
		if (std::abs(px) + std::abs(py) > std::abs(x) + std::abs(y)) {
			x = px;
			y = py;
		}
		x = std::max(-127, std::min(127, x));
		y = std::max(-127, std::min(127, y));
		a8host_pot[2 * p] = (unsigned char)(114 + x * 106 / 127);
		a8host_pot[2 * p + 1] = (unsigned char)(114 + y * 106 / 127);
	}

	int key = keyCode.load();
	if (key >= 0)
		keyFrames.fetch_add(1);
	else if (tapLeft.load() > 0) {
		// a key let go before the console saw it: it gets its frames
		key = tapCode.load();
		tapLeft.fetch_sub(1);
	}
	if (resetPulse.load() > 0) {
		key = A52_KEY_RESET;
		resetPulse.fetch_sub(1);
	}
	INPUT_key_code = key >= 0 && key < A52_KEY_COUNT ? akey[key] : AKEY_NONE;
	INPUT_key_shift = fire2 ? 1 : 0;
}

void SessionImpl::AfterFrame()
{
	a52_fujinet_frame(cart);
	RefreshView();
	session_debug_after_frame();
}

void SessionImpl::OnLog(const char* text)
{
	std::lock_guard<std::mutex> lock(logMtx);
	logText += text;
	if (logText.size() > 64 * 1024)
		logText.erase(0, logText.size() - 48 * 1024);
	if (getenv("A52_LOG"))
		fputs(text, stderr);
}

SessionImpl::SessionImpl(a52session* sess) : s(sess)
{
	for (auto& j : joy)
		j.store(0);
	for (auto& j : joyPad)
		j.store(0);
	for (int p = 0; p < A52_PORTS; p++) {
		padX[p].store(0);
		padY[p].store(0);
		ptrX[p].store(0);
		ptrY[p].store(0);
	}
	A52Host::Callbacks cb;
	cb.init = [this](std::string& err) { return CoreReinit(err); };
	cb.applyConfig = [this] { prefs_emit_options(s, apply_option, nullptr); };
	cb.beforeFrame = [this] { BeforeFrame(); };
	cb.afterFrame = [this] { AfterFrame(); };
	cb.videoOptions = [this](int& a) { a = area.load(); };
	cb.onStopped = [this] { session_debug_stopped(s); };
	cb.onLog = [this](const char* t) { OnLog(t); };
	host.SetCallbacks(cb);
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

// The BoIP endpoint the cartridge dials ("" = FujiNet off).
static std::string boip_target(a52session* s)
{
	return s->fujinet_wanted ? std::string(s->boip_endpoint) : std::string();
}

int a52session_start(a52session* s, const a52session_start_opts* opts)
{
	SessionImpl* im = Impl(s);
	if (s->running)
		return 0;
	s->last_error[0] = 0;
	if (opts)
		s->opts = *opts;
	else
		a52session_default_opts(s, &s->opts);
	s->fujinet_external = 0;
	snprintf(s->boip_endpoint, sizeof s->boip_endpoint, "127.0.0.1:%d", A52SESSION_BOIP_PORT);
	if (s->opts.enable_fujinet && s->opts.external_fujinet) {
		// another FujiNet instance: the cartridge dials where it was told
		char host[64];
		int port;
		if (a52session_parse_hostport(s->opts.fujinet_addr, host, sizeof host, &port) != 0) {
			session_set_error(s, "\"%s\" is not an address:port for the external FujiNet "
			                  "(example: 127.0.0.1:11524)",
			                  s->opts.fujinet_addr ? s->opts.fujinet_addr : "");
			return -1;
		}
		snprintf(s->boip_endpoint, sizeof s->boip_endpoint, "%s:%d", host, port);
		s->fujinet_external = 1;
		s->fujinet_wanted = 1;
	}
	else {
		s->fujinet_wanted = s->opts.enable_fujinet && session_fujinet_available(s);
	}

	// the game to boot: the caller's, else the remembered one
	im->cartImage.clear();
	s->cart_path[0] = 0;
	if (s->opts.cart_path && *s->opts.cart_path) {
		std::string err;
		if (session_load_cart_image(s, s->opts.cart_path, im->cartImage, err) != 0) {
			im->Notice(err);
			im->cartImage.clear();
		}
		else {
			snprintf(s->cart_path, sizeof s->cart_path, "%s", s->opts.cart_path);
		}
	}
	{
		std::lock_guard<std::mutex> lock(im->cartMtx);
		im->cartBoip = boip_target(s);
		im->cartSync = s->opts.sync_cart != 0;
	}

	std::string err;
	// with a built-in FujiNet the console stays off until its listener is up
	im->host.SetPowered(!(s->fujinet_wanted && !s->fujinet_external));
	if (!im->host.Start(err)) {
		session_set_error(s, "%s", err.c_str());
		return -1;
	}
	s->running = 1;
	session_debug_trace_install(s);
	if (s->fujinet_wanted && !s->fujinet_external) {
		if (session_fujinet_start(s) == 0 && session_fujinet_wait_for_boip(s, 4000) != 0)
			im->Notice("FujiNet has not opened its port yet; the cartridge will keep dialling");
		im->host.Call([im] { im->ColdStart(true); });
		im->host.SetPowered(true);
	}
	if (s->opts.enable_gamepad)
		gamepad_start(s);
	if (s->opts.enable_audio && audio_start(s) != 0)
		fprintf(stderr, "a52: %s (continuing without sound)\n", s->last_error);
	im->SnapshotApplied();
	return 0;
}

void a52session_stop(a52session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return;
	audio_stop(s);
	gamepad_stop(s);
	session_fujinet_stop(s);
	im->host.Stop();
	if (im->cart)
		a52_fujinet_power_off(im->cart);
	s->fujinet_external = 0;
	s->running = 0;
}

// ---------------------------------------------------------------------------
// settings
// ---------------------------------------------------------------------------

std::string SessionImpl::PrefValue(int i)
{
	const a52_pref* p = a52session_pref(i);
	if (!p)
		return "";
	if (p->type == A52_PREF_CHOICE) {
		const char* label = a52session_pref_choice(s, i, a52session_pref_get_choice(s, i));
		return std::to_string(a52session_pref_get_choice(s, i)) + ":" + (label ? label : "");
	}
	if (p->type == A52_PREF_FILE || p->type == A52_PREF_DIR || p->type == A52_PREF_TEXT)
		return a52session_get_str(s, p->key, "");
	return std::to_string(a52session_get_int(s, p->key, p->def));
}

void SessionImpl::SnapshotApplied()
{
	applied.clear();
	for (int i = 0; i < a52session_pref_count(); i++)
		applied.push_back(PrefValue(i));
	appliedRoms = RomsSignature();
}

std::string SessionImpl::RomsSignature()
{
	std::string sig;
	for (int i = 0; i < a52session_rom_slot_count(); i++)
		sig += a52session_rom_available(s, i) ? '1' : '0';
	return sig;
}

int a52session_apply_settings(a52session* s)
{
	SessionImpl* im = Impl(s);
	int need = A52_APPLY_LIVE;
	bool restart = false;
	for (int i = 0; i < a52session_pref_count(); i++) {
		std::string v = im->PrefValue(i);
		if (i < (int)im->applied.size() && v == im->applied[i])
			continue;
		const a52_pref* p = a52session_pref(i);
		need = std::max(need, p->applies);
		if (p->applies == A52_APPLY_RESTART)
			restart = true;
	}
	a52session_settings_flush(s);
	if (!s->running) {
		im->SnapshotApplied();
		return need;
	}
	if (restart) {
		a52session_start_opts o;
		a52session_default_opts(s, &o);
		const std::string keep = s->cart_path;
		a52session_stop(s);
		o.cart_path = keep.c_str();
		a52session_start(s, &o);
	}
	else if (need == A52_APPLY_POWER) {
		std::string err;
		im->host.Call([&] {
			if (!im->CoreReinit(err))
				session_set_error(s, "%s", err.c_str());
		});
	}
	else {
		im->host.Call([im] { im->ApplyLive(); });
	}
	im->SnapshotApplied();
	return need;
}

void session_roms_changed(struct a52session* s)
{
	(void)s;  // picked up by the next power cycle (options are rebuilt then)
}

// ---------------------------------------------------------------------------
// power
// ---------------------------------------------------------------------------

// The power cycle takes the console AND the cartridge down: CONFIG is back.
static int power_with_image(a52session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return -1;
	{
		std::lock_guard<std::mutex> lock(im->cartMtx);
		im->cartBoip = boip_target(s);
	}
	im->host.Call([im] { im->ColdStart(true); });
	return 0;
}

int a52session_power_cycle(a52session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return -1;
	{
		std::lock_guard<std::mutex> lock(im->cartMtx);
		im->cartImage.clear();
	}
	s->cart_path[0] = 0;
	a52session_set_str(s, "cart", "");
	return power_with_image(s);
}

int a52session_reboot_to_config(a52session* s)
{
	return a52session_power_cycle(s);
}

// Boot the (already loaded) image the session holds.
int session_power_on_loaded_cart(a52session* s)
{
	return power_with_image(s);
}

int a52session_reset(a52session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running)
		return -1;
	im->resetPulse.store(4);
	return 0;
}

void a52session_set_warp(a52session* s, int on)
{
	SessionImpl* im = Impl(s);
	im->host.SetWarp(on != 0);
	im->host.Post([on] {
		Atari800_refresh_rate = on ? 4 : 1;
		Atari800_collisions_in_skipped_frames = on ? TRUE : FALSE;
	});
	if (!on)
		im->host.FlushAudio();
}

int a52session_warp(a52session* s)
{
	return Impl(s)->host.Warp() ? 1 : 0;
}

void a52session_set_paused(a52session* s, int paused)
{
	Impl(s)->host.SetPaused(paused != 0);
}

int a52session_is_paused(const a52session* s)
{
	return Impl(const_cast<a52session*>(s))->host.Paused() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// video and audio
// ---------------------------------------------------------------------------

int a52session_copy_frame(a52session* s, uint32_t* dst, int* width, int* height,
                           int* par_milli, uint64_t* serial_inout)
{
	int w = 0, h = 0, par = 1000;
	bool fresh = Impl(s)->host.CopyFrame(dst, (size_t)A52SESSION_FB_MAX_WIDTH * A52SESSION_FB_MAX_HEIGHT,
	                                     w, h, par, serial_inout);
	if (width)
		*width = w;
	if (height)
		*height = h;
	if (par_milli)
		*par_milli = par;
	return fresh ? 1 : 0;
}

int a52session_refresh_rate(a52session* s)
{
	return (int)std::lround(Impl(s)->host.Fps() * 1000.0);
}

void a52session_set_frame_callback(a52session* s, a52session_frame_cb cb, void* ctx)
{
	if (s)
		Impl(s)->host.SetFrameCallback(cb, ctx);
}

uint64_t a52session_frame_count(a52session* s)
{
	return s ? Impl(s)->host.FrameCount() : 0;
}

void a52session_notify_vsync(a52session* s, int64_t frame_time_ns)
{
	Impl(s)->host.NotifyVsync(frame_time_ns);
}

int a52session_render_audio(a52session* s, float* stereo, int nframes)
{
	Impl(s)->host.FillAudio(stereo, (uint32_t)nframes);
	return nframes;
}

void a52session_set_volume(a52session* s, int percent)
{
	Impl(s)->host.SetVolume(percent);
}

// ---------------------------------------------------------------------------
// paths, status, notices
// ---------------------------------------------------------------------------

const char* a52session_config_path(const a52session* s) { return s->config_dir; }
const char* a52session_data_path(const a52session* s) { return s->data_dir; }
const char* a52session_roms_path(const a52session* s) { return s->roms_dir; }
const char* a52session_media_path(const a52session* s) { return s->media_dir; }
const char* a52session_sd_path(const a52session* s) { return s->fujinet_sd; }

int a52session_bios_name(a52session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->logMtx);
	if (!dst || dstsz <= 0)
		return 0;
	return snprintf(dst, (size_t)dstsz, "%s", im->biosName.c_str());
}

int a52session_status(a52session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	char bios[64], cart[128], cs[128];
	a52session_bios_name(s, bios, sizeof bios);
	a52session_cart_info(s, cart, sizeof cart);
	const char* tv = a52session_get_int(s, "tv", 0) ? "PAL" : "NTSC";
	std::string fn;
	if (!s->fujinet_wanted)
		fn = "FujiNet off";
	else {
		a52session_cart_status(s, cs, sizeof cs);
		fn = std::string("FujiNet ") + cs;
	}
	std::string extra;
	if (im->host.Warp())
		extra += " \xc2\xb7 Warp";
	if (im->host.Paused())
		extra += " \xc2\xb7 Paused";
	if (im->host.Stopped())
		extra += " \xc2\xb7 Stopped";
	return snprintf(dst, (size_t)dstsz, "%s \xc2\xb7 %s \xc2\xb7 %s \xc2\xb7 %s%s", bios, tv, cart,
	                fn.c_str(), extra.c_str());
}

int a52session_take_notice(a52session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::lock_guard<std::mutex> lock(im->noticeMtx);
	if (im->notice.empty())
		return 0;
	snprintf(dst, (size_t)dstsz, "%s", im->notice.c_str());
	im->notice.clear();
	return 1;
}

void SessionImpl::Notice(const std::string& text)
{
	std::lock_guard<std::mutex> lock(noticeMtx);
	notice = text;
}

// ---- cross-thread system actions ----------------------------------------------

void a52session_sysaction_post(a52session* s, int sysact)
{
	if (sysact < 0 || sysact >= A52_SYSACT_COUNT)
		return;
	pthread_mutex_lock(&s->sysact_mtx);
	s->sysact_pending |= 1u << sysact;
	pthread_mutex_unlock(&s->sysact_mtx);
}

int a52session_sysaction_take(a52session* s, int* out)
{
	int rc = 0;
	pthread_mutex_lock(&s->sysact_mtx);
	for (int i = 0; i < A52_SYSACT_COUNT; i++)
		if (s->sysact_pending & (1u << i)) {
			s->sysact_pending &= ~(1u << i);
			*out = i;
			rc = 1;
			break;
		}
	pthread_mutex_unlock(&s->sysact_mtx);
	return rc;
}

// ---- the cartridge's link and status ----------------------------------------------

int a52session_fujinet_link(a52session* s)
{
	SessionImpl* im = Impl(s);
	if (!s->running || !s->fujinet_wanted || !im->cart)
		return -1;
	a52_cart_status_t st;
	a52_fujinet_status(im->cart, &st);
	return st.link_up ? 1 : 0;
}

int a52session_cart_status(a52session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::string t;
	a52_cart_status_t st = {};
	if (!s->running || !im->cart) {
		t = "not running";
	}
	else {
		a52_fujinet_status(im->cart, &st);
		if (st.link_off)
			t = "off";
		else if (!st.link_up)
			t = s->fujinet_external ? "waiting for the external FujiNet" : "link down";
		else if (st.boot_pct > 0 && st.boot_pct < 100)
			t = "loading " + std::to_string(st.boot_pct) + "%";
		else if (st.booted_game && !st.mailbox)
			t = "connected; game running, mailbox closed";
		else
			t = "connected";
	}
	return snprintf(dst, (size_t)dstsz, "%s", t.c_str());
}

int a52session_cart_booted_game(a52session* s)
{
	SessionImpl* im = Impl(s);
	if (!im->cart)
		return 0;
	a52_cart_status_t st;
	a52_fujinet_status(im->cart, &st);
	return st.booted_game ? 1 : 0;
}

// ---- gamepads ------------------------------------------------------------------

void session_gamepad_event(struct a52session* s, const char* text)
{
	pthread_mutex_lock(&s->pad_event_mtx);
	snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
	pthread_mutex_unlock(&s->pad_event_mtx);
}

int a52session_gamepad_last_event(a52session* s, char* dst, int dstsz)
{
	if (!dst || dstsz <= 0)
		return 0;
	pthread_mutex_lock(&s->pad_event_mtx);
	const int n = snprintf(dst, (size_t)dstsz, "%s", s->pad_event);
	pthread_mutex_unlock(&s->pad_event_mtx);
	return n;
}

int session_active_ports(struct a52session*)
{
	return A52_PORTS;
}

#ifdef A52_GAMEPAD_STUB
int gamepad_start(struct a52session* s)
{
	session_set_error(s, "gamepad support not built");
	return -1;
}
void gamepad_stop(struct a52session*) {}
int a52session_gamepad_count(a52session*) { return 0; }
int a52session_gamepad_name(a52session*, int, char* dst, int dstsz)
{
	if (dst && dstsz > 0)
		dst[0] = '\0';
	return 0;
}
void a52session_gamepad_assign(a52session*, int, int) {}
int a52session_gamepad_assignment(a52session*, int) { return -1; }
int a52session_gamepad_effective_port(a52session*, int) { return -1; }
unsigned a52session_gamepad_generation(a52session*) { return 0; }
void a52session_gamepad_capture_begin(a52session*) {}
void a52session_gamepad_capture_cancel(a52session*) {}
int a52session_gamepad_capture_poll(a52session*, int*) { return 0; }
#endif
