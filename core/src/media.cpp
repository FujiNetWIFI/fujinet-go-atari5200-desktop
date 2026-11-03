/*
 * media.cpp -- game cartridges for the FujiNet cartridge, the SD folder and
 * the router behind Open and drag & drop. The cartridge itself (mappers,
 * planner, mailbox) is core/cart; here a file becomes a cartridge image.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "fujinet_cart.h"
#include "session_impl.h"
#include "zipread.h"

namespace {

// the cartridge's largest image: 128K Super Cart plus a .car header
constexpr uint32_t MaxImage = 4 * 0x8000u + 16;

std::string base_name(const std::string& p)
{
	size_t i = p.find_last_of("/\\");
	return i == std::string::npos ? p : p.substr(i + 1);
}

std::string lower_ext(const std::string& name)
{
	size_t i = name.find_last_of('.');
	if (i == std::string::npos)
		return "";
	std::string e = name.substr(i + 1);
	for (auto& c : e)
		c = (char)tolower((unsigned char)c);
	return e;
}

std::string stem_of(const std::string& name)
{
	size_t i = name.find_last_of('.');
	return i == std::string::npos ? name : name.substr(0, i);
}

bool is_dir(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool read_file(const char* path, std::vector<uint8_t>& out, uint32_t max, std::string& err)
{
	FILE* f = fopen(path, "rb");
	if (!f) {
		err = std::string("cannot open ") + path;
		return false;
	}
	out.clear();
	uint8_t buf[16384];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
		out.insert(out.end(), buf, buf + n);
		if (out.size() > max) {
			fclose(f);
			err = "the file is larger than the cartridge can hold (128 KB)";
			return false;
		}
	}
	fclose(f);
	return true;
}

bool is_cart_ext(const std::string& e)
{
	return e == "a52" || e == "bin" || e == "rom" || e == "car";
}

void set_msg(char* msg, int msgsz, const std::string& text)
{
	if (msg && msgsz > 0)
		snprintf(msg, (size_t)msgsz, "%s", text.c_str());
}

bool copy_bytes(const std::string& dst, const uint8_t* data, size_t n)
{
	FILE* out = fopen(dst.c_str(), "wb");
	if (!out)
		return false;
	bool ok = fwrite(data, 1, n, out) == n;
	return fclose(out) == 0 && ok;
}

// The one cartridge in a zip (or the one file): its bytes and its name.
bool load_one(const char* path, std::vector<uint8_t>& image, std::string& name, std::string& err)
{
	zipread* z = zipread_open(path);
	if (!z) {
		name = base_name(path);
		return read_file(path, image, MaxImage, err);
	}
	std::vector<int> carts, all;
	for (int i = 0; i < zipread_count(z); i++) {
		std::string n = zipread_name(z, i);
		if (!n.empty() && n.back() == '/')
			continue;
		all.push_back(i);
		if (is_cart_ext(lower_ext(base_name(n))))
			carts.push_back(i);
	}
	int pick = -1;
	if (carts.size() == 1)
		pick = carts[0];
	else if (carts.empty() && all.size() == 1)
		pick = all[0];
	if (pick < 0) {
		err = all.empty() ? "the zip is empty"
		                  : "the zip holds " + std::to_string(carts.empty() ? all.size() : carts.size())
		                        + " files; open one cartridge image";
		zipread_close(z);
		return false;
	}
	uint8_t* d = nullptr;
	uint32_t n = 0;
	name = base_name(zipread_name(z, pick));
	if (zipread_size(z, pick) > MaxImage || zipread_extract(z, pick, &d, &n, MaxImage) != 0) {
		err = "the zipped image is larger than the cartridge can hold (128 KB)";
		zipread_close(z);
		return false;
	}
	image.assign(d, d + n);
	free(d);
	zipread_close(z);
	return true;
}

} // namespace

int session_load_cart_image(a52session* s, const char* path, std::vector<uint8_t>& image,
                            std::string& err)
{
	(void)s;
	std::string name;
	if (!path || !*path) {
		err = "no file";
		return -1;
	}
	if (!load_one(path, image, name, err))
		return -1;
	char why[200];
	bool claims;
	if (!a52_fujinet_check_image(image.data(), (uint32_t)image.size(), &claims, nullptr, nullptr,
	                             why, sizeof why)) {
		err = why;
		return -1;
	}
	return 0;
}

int a52session_check_cart(const char* path, char* why, int whysz)
{
	std::vector<uint8_t> image;
	std::string err;
	if (session_load_cart_image(nullptr, path, image, err) != 0) {
		set_msg(why, whysz, err);
		return 0;
	}
	return 1;
}

int a52session_cart_is_app(const char* path)
{
	std::vector<uint8_t> image;
	std::string err;
	bool claims = false;
	if (session_load_cart_image(nullptr, path, image, err) != 0)
		return 0;
	a52_fujinet_check_image(image.data(), (uint32_t)image.size(), &claims, nullptr, nullptr, nullptr, 0);
	return claims ? 1 : 0;
}

int a52session_media_kind(a52session* s, const char* path)
{
	(void)s;
	if (!path || !*path)
		return A52_MEDIA_UNKNOWN;
	if (is_dir(path))
		return roms_identify_file(path) ? A52_MEDIA_BIOS : A52_MEDIA_UNKNOWN;
	if (roms_identify_file(path))
		return A52_MEDIA_BIOS;
	// the extension routes a file (a text file is a size the planner would
	// call a tiny ROM): cartridge images are .a52 .bin .rom .car and zips
	{
		const std::string e = lower_ext(base_name(path));
		if (!is_cart_ext(e) && e != "zip")
			return A52_MEDIA_UNKNOWN;
	}
	return a52session_check_cart(path, nullptr, 0) ? A52_MEDIA_CARTRIDGE : A52_MEDIA_UNKNOWN;
}

int a52session_load_cart(a52session* s, const char* path, char* msg, int msgsz)
{
	SessionImpl* im = Impl(s);
	std::vector<uint8_t> image;
	std::string err;
	if (session_load_cart_image(s, path, image, err) != 0) {
		set_msg(msg, msgsz, err);
		return -1;
	}
	{
		std::lock_guard<std::mutex> lock(im->cartMtx);
		im->cartImage = image;
	}
	snprintf(s->cart_path, sizeof s->cart_path, "%s", path);
	a52session_set_str(s, "cart", path);
	if (s->running && session_power_on_loaded_cart(s) != 0) {
		set_msg(msg, msgsz, "the console is not running");
		return -1;
	}
	char info[160];
	if (s->running)
		a52session_cart_info(s, info, sizeof info);
	else
		snprintf(info, sizeof info, "%s", base_name(path).c_str());
	set_msg(msg, msgsz, std::string("Running ") + base_name(path));
	return 0;
}

int a52session_eject(a52session* s)
{
	return a52session_power_cycle(s);
}

const char* a52session_cart_path(const a52session* s)
{
	return s->cart_path;
}

int a52session_cart_info(a52session* s, char* dst, int dstsz)
{
	SessionImpl* im = Impl(s);
	std::string t;
	a52_cart_status_t st = {};
	if (!s->running || !im->cart) {
		t = "FujiNet cartridge";
	}
	else {
		a52_fujinet_status(im->cart, &st);
		char crc[16];
		snprintf(crc, sizeof crc, "%08X", st.live_crc);
		if (!st.booted_game)
			t = "FujiNet CONFIG";
		else if (st.claim)
			t = "FujiNet app (" + std::to_string(st.live_size / 1024) + " KB, CRC " + crc + ")";
		else
			t = std::string("Cartridge: ") + a52_fujinet_kind_name(st.mapper) + " "
			    + std::to_string(st.live_size / 1024) + " KB (CRC " + crc + ")";
	}
	return snprintf(dst, (size_t)dstsz, "%s", t.c_str());
}

int a52session_import_to_sd(a52session* s, const char* src_path, char* dest_out, int dest_sz)
{
	std::vector<uint8_t> data;
	std::string name, err;
	if (!src_path || is_dir(src_path))
		return -1;
	paths_mkdir_p(s->fujinet_sd);
	zipread* z = zipread_open(src_path);
	if (z) {
		zipread_close(z);
		if (!load_one(src_path, data, name, err))
			return -1;
	}
	else {
		name = base_name(src_path);
		if (!read_file(src_path, data, 64u << 20, err))
			return -1;
	}
	std::string dest = name;
	if (is_cart_ext(lower_ext(name)) && lower_ext(name) != "a52")
		dest = stem_of(name) + ".a52";   // the extension FujiNet mounts as a cartridge
	dest = std::string(s->fujinet_sd) + "/" + dest;
	if (!copy_bytes(dest, data.data(), data.size()))
		return -1;
	if (dest_out && dest_sz > 0)
		snprintf(dest_out, (size_t)dest_sz, "%s", dest.c_str());
	return 0;
}

int a52session_open(a52session* s, const char* path, char* msg, int msgsz)
{
	const int kind = a52session_media_kind(s, path);
	if (kind == A52_MEDIA_BIOS)
		return a52session_rom_import(s, path, -1, msg, msgsz) >= 0 ? 0 : -1;
	if (kind == A52_MEDIA_CARTRIDGE)
		return a52session_load_cart(s, path, msg, msgsz);
	char why[200] = "";
	if (!is_dir(path))
		a52session_check_cart(path, why, sizeof why);
	set_msg(msg, msgsz, why[0] ? std::string(why)
	                           : std::string("That is neither a cartridge image nor a 5200 BIOS"));
	return -1;
}

int a52session_open_or_import(a52session* s, const char* path, char* msg, int msgsz)
{
	const int kind = a52session_media_kind(s, path);
	if (kind != A52_MEDIA_UNKNOWN)
		return a52session_open(s, path, msg, msgsz) == 0 ? kind : -1;
	char dest[1024];
	if (a52session_import_to_sd(s, path, dest, sizeof dest) == 0) {
		set_msg(msg, msgsz, std::string("Copied to the FujiNet SD folder: ") + base_name(dest));
		return 0;
	}
	set_msg(msg, msgsz, "Could not copy that file");
	return -1;
}
