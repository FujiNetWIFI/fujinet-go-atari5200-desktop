/*
 * SessionImpl -- the C++ half of a52session, shared by session.cpp,
 * input.cpp, media.cpp and debug.cpp.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef A52_SESSION_IMPL_H
#define A52_SESSION_IMPL_H

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "A52Host.h"
#include "fujinet_cart.h"
#include "session_internal.h"

struct SessionImpl
{
	explicit SessionImpl(a52session* s);

	a52session* s;
	A52Host host;
	bool coreUp = false;

	// ---- the FujiNet cartridge (created with the core, one per process)
	a52_fujinet_t* cart = nullptr;
	std::mutex cartMtx;                        // image / boip below
	std::vector<uint8_t> cartImage;            // the game the next power-on serves ({} = CONFIG)
	std::string cartBoip;                      // "host:port" the cartridge dials, "" = FujiNet off
	bool cartSync = false;
	std::atomic<unsigned> viewSerial { 0 };    // glue: last a52_fujinet_view_serial copied
	// emulation thread: (re)power the cartridge and cold start the console
	bool PowerOn(std::string& err);

	// ---- settings applied at the last start or apply
	std::vector<std::string> applied;
	std::string appliedRoms;
	std::string PrefValue(int i);
	void SnapshotApplied();
	std::string RomsSignature();

	// ---- emulation-thread work
	bool CoreInit(std::string& err);
	bool CoreReinit(std::string& err);
	void ColdStart(bool initMachine);          // cart + console power on
	void AttachCart();
	void RefreshView();
	void ApplyLive();
	void ApplyVideo();
	void BeforeFrame();
	void AfterFrame();
	void OnLog(const char* text);

	// ---- controllers
	std::atomic<unsigned> joy[A52_PORTS];      // frontends, keyboard bindings: 1 << a52_action
	std::atomic<unsigned> joyPad[A52_PORTS];   // gamepads' digital directions / buttons
	std::atomic<int> padX[A52_PORTS], padY[A52_PORTS];   // gamepad analog -128..127
	std::atomic<int> ptrX[A52_PORTS], ptrY[A52_PORTS];   // a52session_stick_set
	// console keys: the 5200 scans ONE key at a time, the most recent still
	// held; a key let go before a frame saw it is held a few frames (a tap)
	std::mutex keyMtx;
	std::vector<int> heldKeys;                 // a52_key, oldest first
	std::atomic<unsigned> keysMask { 0 };      // 1 << a52_key, for the UI
	std::atomic<int> keyCode { -1 };           // a52_key being held, -1 none
	std::atomic<int> keyFrames { 0 };          // frames the Atari has seen it
	std::atomic<int> tapCode { -1 }, tapLeft { 0 };
	static constexpr int KeyMinFrames = 3;
	std::atomic<int> resetPulse { 0 };         // frames left on a52session_reset's RESET
	std::atomic<bool> analog { true };
	void UpdateKey();                          // keyMtx held

	// ---- video
	std::atomic<int> area { 1 };
	std::string biosName = "Built-in Altirra 5200 OS";   // logMtx

	// ---- log and notices
	std::mutex logMtx;
	std::string logText;
	std::mutex noticeMtx;
	std::string notice;
	void Notice(const std::string& text);
};

SessionImpl* Impl(a52session* s);

// media.cpp: reads a game cartridge image (a file, or a .zip of exactly one)
// and checks the cartridge can run it. 0 or -1 with err.
int session_load_cart_image(a52session* s, const char* path, std::vector<uint8_t>& image,
                            std::string& err);
// session.cpp: power cycle onto the image the session now holds
int session_power_on_loaded_cart(a52session* s);

extern "C" {
/* input.cpp */
int  session_binding_target_for_key(struct a52session* s, uint32_t keysym);
/* debug.cpp */
void session_debug_stopped(struct a52session* s);
void session_debug_pause(struct a52session* s);
void session_debug_free(struct a52session* s);
void session_debug_after_frame(void);
void session_debug_trace_install(struct a52session* s);   // after the cart exists
}

#endif
