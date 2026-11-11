// R2PORT QA: renderer-side frame capture for the visual regression tools (tools\qa).
//
// R2_QA_CAPTURE=<dir> enables it. A watcher thread polls <dir>\capture.req every 50 ms;
// the file holds "<count> <name> [every_n_flips]". The request is consumed (deleted) and
// the next <count> flipped frames (every n-th flip) are read back from the RSX display
// buffer through RPCS3's own screenshot path (g_user_asked_for_screenshot ->
// GSFrameBase::take_screenshot) and written as <dir>\<name>_<i>.png. When all are on disk
// <dir>\<name>.done is written. The image is the game's display buffer before the swapchain
// (no window chrome, no desktop composition, no launcher overlays), at render resolution.
#include "stdafx.h"
#include "util/atomic.hpp"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

extern atomic_t<bool> g_user_asked_for_screenshot;

namespace r2qa
{
	namespace
	{
		std::string s_dir;
		std::mutex s_mtx;
		std::string s_name;
		int s_left = 0, s_index = 0, s_every = 1, s_skip = 0, s_pending_writes = 0;

		u32 s_crc_table[256];
		u32 crc32_png(const u8* p, usz n)
		{
			if (!s_crc_table[1])
				for (u32 i = 0; i < 256; i++)
				{
					u32 c = i;
					for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
					s_crc_table[i] = c;
				}
			u32 c = 0xffffffffu;
			for (usz i = 0; i < n; i++) c = s_crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
			return c ^ 0xffffffffu;
		}
		void be32(std::vector<u8>& v, u32 x)
		{
			v.push_back(u8(x >> 24)); v.push_back(u8(x >> 16)); v.push_back(u8(x >> 8)); v.push_back(u8(x));
		}
		void chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data)
		{
			be32(out, u32(data.size()));
			const usz at = out.size();
			out.insert(out.end(), type, type + 4);
			out.insert(out.end(), data.begin(), data.end());
			be32(out, crc32_png(out.data() + at, data.size() + 4));
		}

		// RGB8 PNG (uncompressed, ~2.7 MB per 720p frame)
		bool write_png(const std::string& path, const u8* px, u32 w, u32 h, bool bgra)
		{
			std::vector<u8> raw;
			raw.reserve(usz(h) * (usz(w) * 3 + 1));
			for (u32 y = 0; y < h; y++)
			{
				raw.push_back(0);
				const u8* row = px + usz(y) * w * 4;
				for (u32 x = 0; x < w; x++)
				{
					const u8* p = row + usz(x) * 4;
					raw.push_back(bgra ? p[2] : p[0]);
					raw.push_back(p[1]);
					raw.push_back(bgra ? p[0] : p[2]);
				}
			}
			// zlib stream of stored (uncompressed) deflate blocks: the vendored zlib is a stub
			std::vector<u8> z = { 0x78, 0x01 };
			z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
			for (usz off = 0;;)
			{
				const usz n = std::min<usz>(65535, raw.size() - off);
				const bool last = off + n >= raw.size();
				z.push_back(last ? 1 : 0);
				z.push_back(u8(n)); z.push_back(u8(n >> 8));
				z.push_back(u8(~n)); z.push_back(u8(~n >> 8));
				z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
				off += n;
				if (last) break;
			}
			u32 a = 1, b = 0;
			for (usz i = 0; i < raw.size();)
			{
				const usz e = std::min<usz>(raw.size(), i + 5552);
				for (; i < e; i++) { a += raw[i]; b += a; }
				a %= 65521; b %= 65521;
			}
			be32(z, (b << 16) | a);

			std::vector<u8> out = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
			std::vector<u8> ihdr;
			be32(ihdr, w); be32(ihdr, h);
			ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 });
			chunk(out, "IHDR", ihdr);
			chunk(out, "IDAT", z);
			chunk(out, "IEND", {});

			const std::string tmp = path + ".part";
			FILE* f = std::fopen(tmp.c_str(), "wb");
			if (!f) return false;
			const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
			std::fclose(f);
			return ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
		}

		void finish_if_done()
		{
			// caller holds s_mtx
			if (s_left > 0 || s_pending_writes > 0 || s_name.empty()) return;
			const std::string done = s_dir + "\\" + s_name + ".done";
			if (FILE* f = std::fopen(done.c_str(), "w")) { std::fprintf(f, "%d\n", s_index); std::fclose(f); }
			std::fprintf(stderr, "[r2qa] capture '%s' done: %d frame(s)\n", s_name.c_str(), s_index);
			s_name.clear();
		}

		void watcher()
		{
			const std::string req = s_dir + "\\capture.req";
			for (;;)
			{
				Sleep(50);
				FILE* f = std::fopen(req.c_str(), "r");
				if (!f) continue;
				int count = 0, every = 1;
				char name[128] = {};
				const int got = std::fscanf(f, "%d %127s %d", &count, name, &every);
				std::fclose(f);
				DeleteFileA(req.c_str());
				if (got < 2 || count <= 0) { std::fprintf(stderr, "[r2qa] bad capture.req (want: <count> <name> [every])\n"); continue; }
				std::lock_guard lk(s_mtx);
				if (!s_name.empty()) { std::fprintf(stderr, "[r2qa] capture '%s' still running; request dropped\n", s_name.c_str()); continue; }
				s_name = name; s_left = count; s_index = 0; s_every = every > 0 ? every : 1; s_skip = 0;
				std::fprintf(stderr, "[r2qa] capturing %d frame(s) as '%s' (every %d flip(s))\n", count, name, s_every);
				g_user_asked_for_screenshot = true;
			}
		}
	}

	static void start()
	{
		const char* e = std::getenv("R2_QA_CAPTURE");
		if (!e || !*e) return;
		s_dir = e;
		CreateDirectoryA(s_dir.c_str(), nullptr);
		std::thread(watcher).detach();
		std::fprintf(stderr, "[r2qa] frame capture armed: write \"<count> <name> [every]\" to %s\\capture.req\n", s_dir.c_str());
	}

	static const int s_started = (start(), 0);

	// GSFrameBase::take_screenshot (RSX thread): rgba/bgra rows, width*4 bytes each
	void on_screenshot(std::vector<u8>&& data, u32 w, u32 h, bool bgra)
	{
		if (s_dir.empty()) return;
		std::string path;
		{
			std::lock_guard lk(s_mtx);
			if (s_name.empty() || s_left <= 0) return;
			if (s_skip > 0) { s_skip--; g_user_asked_for_screenshot = true; return; }
			char leaf[192];
			std::snprintf(leaf, sizeof leaf, "%s_%02d.png", s_name.c_str(), s_index);
			path = s_dir + "\\" + leaf;
			s_index++; s_left--; s_pending_writes++;
			s_skip = s_every - 1;
			if (s_left > 0) g_user_asked_for_screenshot = true;
		}
		if (data.size() < usz(w) * h * 4) { std::lock_guard lk(s_mtx); s_pending_writes--; finish_if_done(); return; }
		std::thread([path, d = std::move(data), w, h, bgra]()
		{
			const bool ok = write_png(path, d.data(), w, h, bgra);
			if (!ok) std::fprintf(stderr, "[r2qa] FAILED writing %s\n", path.c_str());
			std::lock_guard lk(s_mtx);
			s_pending_writes--;
			finish_if_done();
		}).detach();
	}
}
