/*
 * input.cpp -- the 5200's controllers: the analog stick, fire and fire 2 on
 * each of four ports, the 12-key pad and START / PAUSE / RESET, as the
 * frontends' targets, the keyboard's bindings, the gamepads' sticks and the
 * Controllers window's pointer.
 *
 * Every way of holding an input is a source of its own (input_merge.h) and the
 * console sees their merge, under the session's input mutex (a leaf lock).
 * So a key release, a window drag letting go or an unplugged pad can only
 * ever release what that source itself held.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "session_impl.h"

// ---- merge (inputMtx held) ------------------------------------------------------------

// The stick's directions on a port: every source's bits, opposed pairs cancelled
// (a CX52's switches cannot close both ways).
unsigned SessionImpl::MergedJoy(int port) const
{
	unsigned bits = 0;
	if (port < 0 || port >= A52_PORTS)
		return 0;
	for (int src = 0; src < A52_SRC_COUNT; src++)
		bits |= joySrc[port][src];
	const unsigned vert = (1u << A52_ACT_UP) | (1u << A52_ACT_DOWN);
	const unsigned horz = (1u << A52_ACT_LEFT) | (1u << A52_ACT_RIGHT);
	if ((bits & vert) == vert)
		bits &= ~vert;
	if ((bits & horz) == horz)
		bits &= ~horz;
	return bits;
}

// The analog stick of a port: the strongest deflection of any source. 0,0 when
// nobody deflects it (the digital bits are the caller's to add).
void SessionImpl::MergedStick(int port, int& xpos, int& ypos) const
{
	int best = 0;
	xpos = ypos = 0;
	if (port < 0 || port >= A52_PORTS)
		return;
	for (int src = 0; src < A52_SRC_COUNT; src++) {
		const int mag = std::abs(stickX[src][port]) + std::abs(stickY[src][port]);
		if (mag > best) {
			best = mag;
			xpos = stickX[src][port];
			ypos = stickY[src][port];
		}
	}
}

// The console keys: the most recent one still held.
// Keyboard lock held. A key pressed and let go within a frame or two (a fast
// tap, a synthetic one) would never be seen: it stays as a tap until the
// console has had it for KeyMinFrames.
void SessionImpl::UpdateKey()
{
	const int was = keyCode.load();
	const int now = heldKeys.empty() ? -1 : heldKeys.back();
	if (now != was) {
		if (now < 0 && keyFrames.load() < KeyMinFrames) {
			tapCode.store(was);
			tapLeft.store(KeyMinFrames - keyFrames.load());
		}
		else if (now >= 0)
			tapLeft.store(0);
		keyFrames.store(0);
	}
	keyCode.store(now);
	unsigned mask = 0;
	for (int key : heldKeys)
		mask |= 1u << key;
	keysMask.store(mask);
}

// heldKeys (oldest first) from what the sources hold now.
void SessionImpl::RecomputeKeys()
{
	unsigned mask = 0;
	for (int src = 0; src < A52_SRC_COUNT; src++)
		mask |= keySrc[src];
	heldKeys.erase(std::remove_if(heldKeys.begin(), heldKeys.end(),
	                              [&](int key) { return !(mask & (1u << key)); }),
	               heldKeys.end());
	for (int key = 0; key < A52_KEY_COUNT; key++)
		if ((mask & (1u << key)) &&
		    std::find(heldKeys.begin(), heldKeys.end(), key) == heldKeys.end())
			heldKeys.push_back(key);
	UpdateKey();
}

// One source's hold on one target. inputMtx held.
static void press_locked(SessionImpl* impl, int src, int target, int down)
{
	if (target < A52_TARGET_KEY(0)) {
		const int port = target / A52_ACT_PER_PORT, act = target % A52_ACT_PER_PORT;
		if (down)
			impl->joySrc[port][src] |= 1u << act;
		else
			impl->joySrc[port][src] &= ~(1u << act);
	}
	else {
		const int key = target - A52_TARGET_KEY(0);
		if (down)
			impl->keySrc[src] |= 1u << key;
		else
			impl->keySrc[src] &= ~(1u << key);
		impl->RecomputeKeys();
	}
}

// Everything the given source holds, let go. inputMtx held.
static void clear_source_locked(SessionImpl* impl, int src)
{
	for (int port = 0; port < A52_PORTS; port++) {
		impl->joySrc[port][src] = 0;
		impl->stickX[src][port] = 0;
		impl->stickY[src][port] = 0;
	}
	impl->keySrc[src] = 0;
	impl->RecomputeKeys();
}

extern "C" void input_press(struct a52session* s, int src, int target, int down)
{
	SessionImpl* impl = Impl(s);
	if (src < 0 || src >= A52_SRC_COUNT || target < 0 || target >= A52_TARGET_SYSACT(0))
		return;
	std::lock_guard<std::mutex> lock(impl->inputMtx);
	press_locked(impl, src, target, down);
}

extern "C" void input_stick(struct a52session* s, int src, int port, int xpos, int ypos)
{
	SessionImpl* impl = Impl(s);
	if (src < 0 || src >= A52_SRC_COUNT || port < 0 || port >= A52_PORTS)
		return;
	std::lock_guard<std::mutex> lock(impl->inputMtx);
	impl->stickX[src][port] = std::max(-127, std::min(127, xpos));
	impl->stickY[src][port] = std::max(-127, std::min(127, ypos));
}

extern "C" void input_pad_clear(struct a52session* s, int owner)
{
	SessionImpl* impl = Impl(s);
	if (owner < 0 || owner >= A52_MAX_PADS)
		return;
	std::lock_guard<std::mutex> lock(impl->inputMtx);
	clear_source_locked(impl, A52_SRC_PAD(owner));
	clear_source_locked(impl, A52_SRC_STICK(owner));
}

// ---- public: the Controllers window's pointer ----------------------------------------

void a52session_press(a52session* s, int target, int down)
{
	if (target < 0 || target >= A52_TARGET_COUNT)
		return;
	if (target < A52_TARGET_SYSACT(0))
		input_press(s, A52_SRC_POINTER, target, down);
	else if (down)
		a52session_sysaction(s, target - A52_TARGET_SYSACT(0));
}

void a52session_sysaction(a52session* s, int sysact)
{
	switch (sysact) {
	case A52_SYSACT_POWER:
		a52session_power_cycle(s);
		break;
	case A52_SYSACT_WARP:
		a52session_set_warp(s, !a52session_warp(s));
		break;
	case A52_SYSACT_PAUSE:
		session_debug_pause(s);
		break;
	default:
		break;
	}
}

// Lets go of the window's pointer: every part and the stick it holds. The
// keyboard and the gamepads are left alone.
void a52session_release_pointer(a52session* s)
{
	SessionImpl* impl = Impl(s);
	std::lock_guard<std::mutex> lock(impl->inputMtx);
	clear_source_locked(impl, A52_SRC_POINTER);
}

// Lets go of the keyboard (focus loss). The window's pointer and every gamepad
// are left alone: each knows when it lets go.
void a52session_release_all(a52session* s)
{
	SessionImpl* impl = Impl(s);
	std::lock_guard<std::mutex> lock(impl->inputMtx);
	memset(impl->heldKeysym, 0, sizeof impl->heldKeysym);
	clear_source_locked(impl, A52_SRC_KEYBOARD);
}

// The keyboard's and the pointer's holds, both (the gamepads' stay).
void a52session_clear_held(a52session* s)
{
	a52session_release_all(s);
	a52session_release_pointer(s);
}

unsigned a52session_buttons_held(a52session* s, int port)
{
	SessionImpl* impl = Impl(s);
	if (port < 0 || port >= A52_PORTS)
		return 0;
	std::lock_guard<std::mutex> lock(impl->inputMtx);
	return impl->MergedJoy(port);
}

unsigned a52session_keys_held(a52session* s)
{
	SessionImpl* impl = Impl(s);
	unsigned mask = impl->keysMask.load();
	if (impl->resetPulse.load() > 0)
		mask |= 1u << A52_KEY_RESET;
	return mask;
}

int a52session_target_held(a52session* s, int target)
{
	if (target >= 0 && target < A52_TARGET_KEY(0))
		return (a52session_buttons_held(s, target / A52_ACT_PER_PORT)
		        >> (target % A52_ACT_PER_PORT)) & 1;
	if (target >= A52_TARGET_KEY(0) && target < A52_TARGET_SYSACT(0))
		return (a52session_keys_held(s) >> (target - A52_TARGET_KEY(0))) & 1;
	return 0;
}

void a52session_stick_get(a52session* s, int port, int* xpos, int* ypos)
{
	SessionImpl* impl = Impl(s);
	int ax = 0, ay = 0;
	if (port >= 0 && port < A52_PORTS) {
		std::lock_guard<std::mutex> lock(impl->inputMtx);
		impl->MergedStick(port, ax, ay);
		if (ax == 0 && ay == 0) {
			// nobody deflects it: the digital directions are full deflection
			const unsigned bits = impl->MergedJoy(port);
			ax = ((bits & (1u << A52_ACT_RIGHT)) ? 127 : 0) - ((bits & (1u << A52_ACT_LEFT)) ? 127 : 0);
			ay = ((bits & (1u << A52_ACT_DOWN)) ? 127 : 0) - ((bits & (1u << A52_ACT_UP)) ? 127 : 0);
		}
	}
	if (xpos)
		*xpos = ax;
	if (ypos)
		*ypos = ay;
}

// The window's drag is a source of its own: letting go (0,0) cancels the drag
// and nothing else.
void a52session_stick_set(a52session* s, int port, int xpos, int ypos)
{
	input_stick(s, A52_SRC_POINTER, port, xpos, ypos);
}

void a52session_set_analog(a52session* s, int on)
{
	Impl(s)->analog.store(on != 0);
	a52session_set_int(s, "analog_stick", on != 0);
}

// ---- keyboard translation --------------------------------------------------------

int a52session_key(a52session* s, uint32_t keysym, int down)
{
	SessionImpl* impl = Impl(s);
	// bindings are case-folded; so is what a held key is remembered by
	const uint32_t folded = keysym >= 'A' && keysym <= 'Z' ? keysym + 32 : keysym;
	const int target = session_binding_target_for_key(s, keysym);   // before the input lock
	std::lock_guard<std::mutex> lock(impl->inputMtx);
	if (!down) {
		// a key lets go of exactly what its press asserted, whatever it is bound to now
		bool was = false;
		for (int tgt = 0; tgt < A52_TARGET_SYSACT(0); tgt++)
			if (keysym && impl->heldKeysym[tgt] == folded) {
				impl->heldKeysym[tgt] = 0;
				press_locked(impl, A52_SRC_KEYBOARD, tgt, 0);
				was = true;
			}
		return was || target >= 0;
	}
	if (target < 0)
		return 0;
	if (impl->heldKeysym[target] != folded) {   // auto-repeat asserts nothing new
		impl->heldKeysym[target] = folded;
		press_locked(impl, A52_SRC_KEYBOARD, target, 1);
	}
	return 1;
}
