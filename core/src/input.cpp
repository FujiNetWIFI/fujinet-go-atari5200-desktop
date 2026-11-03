/*
 * input.cpp -- the 5200's controllers: the analog stick, fire and fire 2 on
 * each of four ports, the 12-key pad and START / PAUSE / RESET, as the
 * frontends' targets, the gamepads' analog sticks and the on-screen panel.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "session_impl.h"

// ---- the console keys: the most recent one still held -------------------------------

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
	for (int k : heldKeys)
		mask |= 1u << k;
	keysMask.store(mask);
}

void a52session_press(a52session* s, int target, int down)
{
	SessionImpl* im = Impl(s);
	if (target < 0 || target >= A52_TARGET_COUNT)
		return;
	if (target < A52_TARGET_KEY(0)) {
		const int port = target / A52_ACT_PER_PORT, act = target % A52_ACT_PER_PORT;
		if (down)
			im->joy[port].fetch_or(1u << act);
		else
			im->joy[port].fetch_and(~(1u << act));
	}
	else if (target < A52_TARGET_SYSACT(0)) {
		const int key = target - A52_TARGET_KEY(0);
		std::lock_guard<std::mutex> lock(im->keyMtx);
		im->heldKeys.erase(std::remove(im->heldKeys.begin(), im->heldKeys.end(), key),
		                   im->heldKeys.end());
		if (down)
			im->heldKeys.push_back(key);
		im->UpdateKey();
	}
	else if (down) {
		a52session_sysaction(s, target - A52_TARGET_SYSACT(0));
	}
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

void a52session_release_all(a52session* s)
{
	SessionImpl* im = Impl(s);
	for (auto& j : im->joy)
		j.store(0);
	for (int p = 0; p < A52_PORTS; p++) {
		im->ptrX[p].store(0);
		im->ptrY[p].store(0);
	}
	std::lock_guard<std::mutex> lock(im->keyMtx);
	im->heldKeys.clear();
	im->UpdateKey();   // a pending tap (already let go) still lands
}

unsigned a52session_buttons_held(a52session* s, int port)
{
	SessionImpl* im = Impl(s);
	if (port < 0 || port >= A52_PORTS)
		return 0;
	return im->joy[port].load() | im->joyPad[port].load();
}

unsigned a52session_keys_held(a52session* s)
{
	SessionImpl* im = Impl(s);
	unsigned m = im->keysMask.load();
	if (im->resetPulse.load() > 0)
		m |= 1u << A52_KEY_RESET;
	return m;
}

void a52session_stick_get(a52session* s, int port, int* x, int* y)
{
	SessionImpl* im = Impl(s);
	int ax = 0, ay = 0;
	if (port >= 0 && port < A52_PORTS) {
		const unsigned b = im->joy[port].load() | im->joyPad[port].load();
		int dx = ((b & (1u << A52_ACT_RIGHT)) ? 127 : 0) - ((b & (1u << A52_ACT_LEFT)) ? 127 : 0);
		int dy = ((b & (1u << A52_ACT_DOWN)) ? 127 : 0) - ((b & (1u << A52_ACT_UP)) ? 127 : 0);
		const int px = im->padX[port].load(), py = im->padY[port].load();
		const int qx = im->ptrX[port].load(), qy = im->ptrY[port].load();
		ax = px;
		ay = py;
		if (std::abs(qx) + std::abs(qy) > std::abs(ax) + std::abs(ay)) {
			ax = qx;
			ay = qy;
		}
		if (ax == 0 && ay == 0) {
			ax = dx;
			ay = dy;
		}
	}
	if (x)
		*x = ax;
	if (y)
		*y = ay;
}

void a52session_stick_set(a52session* s, int port, int x, int y)
{
	SessionImpl* im = Impl(s);
	if (port < 0 || port >= A52_PORTS)
		return;
	im->ptrX[port].store(std::max(-127, std::min(127, x)));
	im->ptrY[port].store(std::max(-127, std::min(127, y)));
}

void session_gamepad_stick(struct a52session* s, int port, int x, int y)
{
	SessionImpl* im = Impl(s);
	if (port < 0 || port >= A52_PORTS)
		return;
	im->padX[port].store(std::max(-127, std::min(127, x)));
	im->padY[port].store(std::max(-127, std::min(127, y)));
}

void a52session_set_analog(a52session* s, int on)
{
	Impl(s)->analog.store(on != 0);
	a52session_set_int(s, "analog_stick", on != 0);
}

void session_gamepad_apply(struct a52session* s, int port, int act, int down)
{
	SessionImpl* im = Impl(s);
	if (port < 0 || port >= A52_PORTS || act < 0 || act >= A52_ACT_PER_PORT)
		return;
	if (down)
		im->joyPad[port].fetch_or(1u << act);
	else
		im->joyPad[port].fetch_and(~(1u << act));
}

// ---- keyboard translation --------------------------------------------------------

int a52session_key(a52session* s, uint32_t keysym, int down)
{
	const int target = session_binding_target_for_key(s, keysym);
	if (target < 0)
		return 0;
	a52session_press(s, target, down);
	return 1;
}

// ---- the controller as printed ---------------------------------------------------

const a52_keycap* a52session_keyboard_layout(int* n, float* width, float* height)
{
	static const char* const labels[A52_KEY_COUNT] = {
		"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#", "START", "PAUSE", "RESET"};
	static a52_keycap caps[A52_KEY_COUNT];
	static bool built;
	if (!built) {
		// START PAUSE RESET along the top, then the 3 x 4 pad
		for (int k = 0; k < A52_KEY_COUNT; k++) {
			a52_keycap& c = caps[k];
			c.label = labels[k];
			c.key = k;
			if (k >= A52_KEY_START) {
				c.x = (float)(k - A52_KEY_START);
				c.y = 0.0f;
				c.w = 1.0f;
				c.h = 0.6f;
				c.kind = A52_KEYCAP_FUNCTION;
			}
			else {
				// 1 2 3 / 4 5 6 / 7 8 9 / * 0 #
				int row, col;
				if (k == A52_KEY_STAR || k == A52_KEY_0 || k == A52_KEY_HASH) {
					row = 3;
					col = k == A52_KEY_STAR ? 0 : k == A52_KEY_0 ? 1 : 2;
				}
				else {
					row = (k - 1) / 3;
					col = (k - 1) % 3;
				}
				c.x = (float)col;
				c.y = 0.8f + (float)row;
				c.w = 1.0f;
				c.h = 1.0f;
				c.kind = A52_KEYCAP_NORMAL;
			}
		}
		built = true;
	}
	if (n)
		*n = A52_KEY_COUNT;
	if (width)
		*width = 3.0f;
	if (height)
		*height = 4.8f;
	return caps;
}
