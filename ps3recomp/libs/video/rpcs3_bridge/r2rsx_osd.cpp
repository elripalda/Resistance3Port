// r2rsx_osd: port UI drawn inside the swapchain image (see r2rsx_bridge.h, "OSD layers").
// The port paints its panels with GDI into 32-bit DIBs as before and hands the pixels here; one
// RPCS3 overlay (Overlays/overlays.h) draws every visible layer as a textured quad when the
// frame is presented (VKPresent.cpp render_overlays). Unlike topmost layered windows this shows
// in exclusive fullscreen too. Built into rpcs3_rsx next to r2rsx_bridge.cpp. GPLv2, like RPCS3.
#include "stdafx.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Overlays/overlays.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/VK/VKR2Hdr.h"

#include "r2rsx_bridge.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

extern "C" volatile int g_pc_hdr_active; // VKR2PcExtras.cpp: 0 SDR, 1 HDR10 (PQ), 2 scRGB

namespace
{
	constexpr int k_slots = R2RSX_OSD_SLOTS;

	// What the port last asked for; written by the port threads, read by the render thread.
	struct pending_layer
	{
		std::vector<u8> px; // BGRA, straight alpha, w * 4 pitch, already HDR-encoded
		int w = 0, h = 0;
		float x = 0, y = 0, ref_w = 1, ref_h = 1, alpha = 1, gain = 1;
		bool visible = false;
		u32 gen = 0;
	};

	struct osd_image : rsx::overlays::image_info_base
	{
		std::vector<u8> buf;
		const u8* get_data() const override { return buf.data(); }
	};

	// The render thread's copy of one layer; its address is the texture cache key (VKOverlays.cpp
	// find_temp_image), so a size change needs a new object and a cache flush.
	struct shown_layer
	{
		std::unique_ptr<osd_image> img;
		u32 gen = 0;
	};

	std::mutex s_mtx;
	pending_layer s_pending[k_slots];
	u32 s_attached_uid = umax;
	bool s_attaching = false;
	std::atomic<int> s_suppress{0};
	std::atomic<unsigned> s_presents{0};
	std::atomic<int> s_warm{0};
	std::atomic<unsigned> s_change{0}, s_drawn{0}; // change counter; the value the last present drew
	unsigned s_warm_until = 0; // under s_mtx: hide the warm-up layer once this many presents went by

	int mode_from_env()
	{
		static const int mode = []
		{
			const char* v = std::getenv("PORT_OSD");
			const int m = (v && (!_stricmp(v, "layered") || !_stricmp(v, "window") || !_stricmp(v, "off") || !std::strcmp(v, "0"))) ? 0 : 1;
			std::printf("[r2rsx] PORT_OSD=%s: port notifications and menus are drawn %s\n",
				m ? "swapchain" : "layered", m ? "into the game's swapchain image" : "as layered windows (not shown in exclusive fullscreen)");
			std::fflush(stdout);
			return m;
		}();
		return mode;
	}

	rsx::overlays::display_manager* manager()
	{
		auto* thr = rsx::get_current_renderer();
		return thr ? thr->overlay_manager() : nullptr;
	}

	// While the game is parked (pause menus) nothing flips; ask the RSX thread for a native UI
	// flip (RSXThread.cpp do_local_task) unless a flip happened recently anyway.
	void kick(u64 stale_us)
	{
		auto* thr = rsx::get_current_renderer();
		if (!thr || !thr->overlay_manager()) return;
		auto& disp = thr->display_ctrl();
		if (disp.get_last_host_flip_timestamp() + stale_us < get_system_time())
			disp.request_native_ui_flip();
	}

	// sRGB 8-bit -> HDR encoding for the current swapchain (the overlay pass writes values as is).
	struct hdr_luts
	{
		float lin[256];
		u8 pq[4097];
		float paper = -1;
	};

	float pq_encode(float nits)
	{
		const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
		const float y = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), m1);
		return std::pow((c1 + c2 * y) / (1.f + c3 * y), m2);
	}

	const hdr_luts& luts(float paper)
	{
		static hdr_luts t;
		if (t.paper != paper)
		{
			for (int i = 0; i < 256; ++i)
			{
				const float c = i / 255.f;
				t.lin[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
			}
			for (int i = 0; i <= 4096; ++i)
				t.pq[i] = static_cast<u8>(std::lround(pq_encode(i / 4096.f * paper) * 255.f));
			t.paper = paper;
		}
		return t;
	}

	// Returns the colour gain the overlay pass applies (scRGB: 1.0 = 80 nits).
	float encode_hdr(std::vector<u8>& px, int hdr)
	{
		if (hdr != 1 && hdr != 2) return 1.f;
		const float paper = std::clamp(r2hdr::paper(), 80.f, 2000.f);
		const hdr_luts& t = luts(paper);
		for (usz i = 0; i + 3 < px.size(); i += 4)
		{
			const float b = t.lin[px[i]], g = t.lin[px[i + 1]], r = t.lin[px[i + 2]];
			if (hdr == 1)
			{
				// BT.709 -> BT.2020 primaries, then PQ at paper white
				const float r2 = 0.6274f * r + 0.3293f * g + 0.0433f * b;
				const float g2 = 0.0691f * r + 0.9195f * g + 0.0114f * b;
				const float b2 = 0.0164f * r + 0.0880f * g + 0.8956f * b;
				px[i + 2] = t.pq[std::clamp(static_cast<int>(r2 * 4096.f + 0.5f), 0, 4096)];
				px[i + 1] = t.pq[std::clamp(static_cast<int>(g2 * 4096.f + 0.5f), 0, 4096)];
				px[i + 0] = t.pq[std::clamp(static_cast<int>(b2 * 4096.f + 0.5f), 0, 4096)];
			}
			else
			{
				px[i + 2] = static_cast<u8>(std::lround(r * 255.f));
				px[i + 1] = static_cast<u8>(std::lround(g * 255.f));
				px[i + 0] = static_cast<u8>(std::lround(b * 255.f));
			}
		}
		return hdr == 2 ? paper / 80.f : 1.f;
	}

	struct port_osd final : rsx::overlays::overlay
	{
		u16 vw = 1280, vh = 720;
		shown_layer shown[k_slots];
		std::vector<std::unique_ptr<osd_image>> graveyard;
		unsigned drawn_change = 0;

		port_osd()
		{
			use_window_space = true;
			visible = true;
		}

		void set_render_viewport(u16 w, u16 h) override { vw = std::max<u16>(w, 1); vh = std::max<u16>(h, 1); }
		u16 get_virtual_width() const override { return vw; }
		u16 get_virtual_height() const override { return vh; }

		rsx::overlays::compiled_resource get_compiled() override
		{
			rsx::overlays::compiled_resource out;
			std::lock_guard lock(s_mtx);

			// Images replaced in the previous frame; the cache flush has run since (ui_overlay_renderer::run)
			graveyard.clear();

			if (uid != s_attached_uid)
				return out;

			drawn_change = s_change.load();
			const bool hide_all = s_suppress.load() != 0;
			for (int i = 0; i < k_slots; ++i)
			{
				const pending_layer& p = s_pending[i];
				shown_layer& s = shown[i];
				if (!p.visible || p.w <= 0 || p.h <= 0 || p.px.empty())
					continue;

				if (!s.img || s.img->w != p.w || s.img->h != p.h)
				{
					if (s.img)
					{
						graveyard.push_back(std::move(s.img));
						status_flags |= rsx::overlays::status_bits::invalidate_image_cache;
					}
					s.img = std::make_unique<osd_image>();
					s.img->w = p.w;
					s.img->h = p.h;
					s.img->bpp = 4;
					s.img->channels = 4;
					s.gen = p.gen - 1;
				}
				if (s.gen != p.gen)
				{
					s.img->buf = p.px;
					s.img->dirty = true;
					s.gen = p.gen;
				}

				if (hide_all || p.alpha <= 0.f)
					continue;

				const f32 sx = vw / std::max(p.ref_w, 1.f), sy = vh / std::max(p.ref_h, 1.f);
				const f32 x0 = p.x * sx, y0 = p.y * sy, x1 = (p.x + p.w) * sx, y1 = (p.y + p.h) * sy;

				rsx::overlays::compiled_resource::command cmd;
				cmd.config.primitives = rsx::overlays::primitive_type::quad_list;
				cmd.config.texture_ref = rsx::overlays::image_resource_id::raw_image;
				cmd.config.external_data_ref = s.img.get();
				cmd.config.color = { p.gain, p.gain, p.gain, std::clamp(p.alpha, 0.f, 1.f) };
				cmd.config.disable_vertex_snap = false;
				cmd.verts.resize(4);
				cmd.verts[0].vec4(x0, y0, 0.f, 0.f);
				cmd.verts[1].vec4(x1, y0, 1.f, 0.f);
				cmd.verts[2].vec4(x0, y1, 0.f, 1.f);
				cmd.verts[3].vec4(x1, y1, 1.f, 1.f);
				out.append(cmd);
			}
			return out;
		}

		void update(u64 /*timestamp_us*/) override
		{
			const unsigned n = s_presents.fetch_add(1) + 1;
			s_drawn.store(drawn_change);
			std::lock_guard lock(s_mtx);
			if (uid != s_attached_uid)
				return;
			if (s_warm_until && n >= s_warm_until)
			{
				s_pending[k_slots - 1].visible = false;
				s_warm_until = 0;
			}
			bool any = false;
			for (const auto& p : s_pending) any |= p.visible;
			if (!any)
			{
				// Nothing to draw: leave the list so the present skips the overlay pass entirely.
				// The manager is locked by the caller, so this is queued until it unlocks.
				s_attached_uid = umax;
				if (auto* m = manager()) m->remove(uid);
			}
		}
	};

	void attach_if_needed()
	{
		{
			std::lock_guard lock(s_mtx);
			if (s_attached_uid != umax || s_attaching)
				return;
			s_attaching = true;
		}
		u32 uid = umax;
		if (auto* m = manager())
		{
			auto entry = std::make_shared<port_osd>();
			uid = m->add(entry)->uid; // replaces a previous port_osd still queued for removal
		}
		std::lock_guard lock(s_mtx);
		s_attached_uid = uid;
		s_attaching = false;
	}
}

extern "C" int r2rsx_osd_wanted(void)
{
	return r2rsx_enabled() && mode_from_env();
}

extern "C" int r2rsx_osd_ready(void)
{
	if (!r2rsx_osd_wanted()) return 0;
	auto* thr = rsx::get_current_renderer();
	if (!thr || !thr->overlay_manager()) return 0;
	if (thr->display_ctrl().get_last_host_flip_timestamp() == 0) return 0; // no swapchain present yet
	if (!s_warm.exchange(1))
	{
		// Compile the overlay pipeline and texture path now, not on the first real notification:
		// a transparent 1x1 layer for a few presents (the warm-up slot is reserved for this).
		const u32 clear = 0;
		r2rsx_osd_layer(R2RSX_OSD_SLOTS - 1, &clear, 1, 1, 0, 0, 1, 1, 1.f);
		std::lock_guard lock(s_mtx);
		s_warm_until = s_presents.load() + 4;
		std::printf("[r2rsx] OSD: swapchain overlay ready (warm-up)\n");
		std::fflush(stdout);
	}
	return 1;
}

extern "C" void r2rsx_osd_layer(int slot, const void* bgra, int w, int h, int x, int y, int ref_w, int ref_h, float alpha)
{
	if (slot < 0 || slot >= k_slots || !r2rsx_osd_wanted()) return;
	std::vector<u8> px;
	float gain = 1.f;
	if (bgra && w > 0 && h > 0)
	{
		px.assign(static_cast<const u8*>(bgra), static_cast<const u8*>(bgra) + static_cast<usz>(w) * h * 4);
		gain = encode_hdr(px, g_pc_hdr_active);
	}
	{
		std::lock_guard lock(s_mtx);
		pending_layer& p = s_pending[slot];
		if (bgra && w > 0 && h > 0)
		{
			p.px.swap(px);
			p.w = w;
			p.h = h;
			p.gain = gain;
			p.gen++;
		}
		p.x = static_cast<float>(x);
		p.y = static_cast<float>(y);
		p.ref_w = static_cast<float>(std::max(ref_w, 1));
		p.ref_h = static_cast<float>(std::max(ref_h, 1));
		p.alpha = alpha;
		p.visible = !p.px.empty();
	}
	s_change.fetch_add(1);
	attach_if_needed();
	kick(33000);
}

extern "C" void r2rsx_osd_hide(int slot)
{
	if (slot < 0 || slot >= k_slots) return;
	{
		std::lock_guard lock(s_mtx);
		if (!s_pending[slot].visible) return;
		s_pending[slot].visible = false;
	}
	s_change.fetch_add(1);
	kick(33000);
}

extern "C" int r2rsx_osd_visible(int slot)
{
	std::lock_guard lock(s_mtx);
	if (slot < 0)
	{
		for (int i = 0; i < k_slots - 1; ++i)
			if (s_pending[i].visible) return 1;
		return 0;
	}
	if (slot >= k_slots) return 0;
	return s_pending[slot].visible ? 1 : 0;
}

extern "C" void r2rsx_osd_suppress(int on)
{
	s_suppress.store(on ? 1 : 0);
	s_change.fetch_add(1);
	kick(0);
}

extern "C" unsigned r2rsx_osd_presents(void)
{
	return s_presents.load();
}

extern "C" void r2rsx_osd_refresh(void)
{
	// A change made while the game is parked shows with the next native flip; ask again if none came.
	if (s_change.load() != s_drawn.load()) kick(33000);
}
