// r2rsx_bridge: drives RPCS3's RSX core from the recompiled runtime (see r2rsx_bridge.h).
// Built as part of the rpcs3_rsx library (third_party/rpcs3/CMakeLists.txt) so it sees the
// RPCS3 headers and compile flags; the game only links the C API. GPLv2, like RPCS3.
#include "stdafx.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/emu_callbacks.h"
#include "Emu/Memory/vm.h"
#include "Emu/Cell/lv2/sys_rsx.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Core/RSXContext.h"
#include "Emu/RSX/Core/RSXRendererFactory.h"
#include "Emu/RSX/port_prof.h"
#include "util/logs.hpp"
#include "util/fixed_typemap.hpp"

#include "r2rsx_bridge.h"

#include <windows.h>
// ---- R2PORT autoconfig (launcher/autoconfig owner): shared R2_PRESET mapping, used only in apply_launcher_overrides ----
#include "../../../../project/r2_autoconfig.h"
// ---- end R2PORT autoconfig ----
#include <tlhelp32.h> // R2PORT perf: R2_RSX_PROF thread times
#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

extern "C" uint8_t* vm_base;
extern "C" uint8_t* vm_sudo_base;
extern "C" int (*g_vm_access_violation_hook)(uint32_t guest_addr, int is_write);
// project/stubs.cpp: R2_FPS target (0 = uncapped) and the display refresh, as main.cpp uses them
extern "C" double r2_fps_target(void);
extern "C" double r2_display_refresh_hz(void);

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern "C" ps3_guest_caller_fn g_ps3_guest_caller;
static bool s_prof_overlay = false; // r2rsx_prof_enable (F3 overlay)

namespace r2port
{
	extern std::string g_rsx_cache_dir;
	extern int g_pipe_compiler_threads; // VK/VKPipelineCompiler.cpp
}

namespace rsx
{
	extern std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;
}

namespace
{
	int s_enabled = -1;
	bool s_up = false;
	rsx::thread* s_render = nullptr;

	HWND s_hwnd = nullptr;
	std::mutex s_win_mtx;
	std::condition_variable s_win_cv;

	std::unique_ptr<logs::listener> s_log;

	// Guest handlers run here, like on RPCS3's _gcm_intr_thread: in order, off the RSX thread.
	std::mutex s_intr_mtx;
	std::condition_variable s_intr_cv;
	std::deque<std::pair<u32, u32>> s_intr_queue;

	void intr_thread_main()
	{
		for (;;)
		{
			std::pair<u32, u32> call;
			{
				std::unique_lock lock(s_intr_mtx);
				s_intr_cv.wait(lock, [] { return !s_intr_queue.empty(); });
				call = s_intr_queue.front();
				s_intr_queue.pop_front();
			}

			if (g_ps3_guest_caller && call.first)
			{
				g_ps3_guest_caller(call.first, call.second, 0, 0, 0, 0, 0, 0, 0);
			}
		}
	}

	class r2_gs_frame final : public GSFrameBase
	{
	public:
		void close() override {}
		void reset() override {}
		video_renderer renderer() const override { return video_renderer::vulkan; }
		bool shown() override { return true; }
		void hide() override {}
		void show() override {}
		void toggle_fullscreen() override {}
		void delete_context(draw_context_t) override {}
		draw_context_t make_context() override { return nullptr; }
		void set_current(draw_context_t) override {}
		void flip(draw_context_t, bool) override {}

		int client_width() override
		{
			RECT r{};
			return GetClientRect(s_hwnd, &r) ? std::max<int>(1, r.right - r.left) : 1280;
		}

		int client_height() override
		{
			RECT r{};
			return GetClientRect(s_hwnd, &r) ? std::max<int>(1, r.bottom - r.top) : 720;
		}

		f64 client_display_rate() override { const double hz = r2_display_refresh_hz(); return hz >= 1.0 ? hz : 60.; }
		bool has_alpha() override { return false; }
		display_handle_t handle() const override { return s_hwnd; }
		bool can_consume_frame() const override { return false; }
		void present_frame(std::vector<u8>&&, u32, u32, u32, bool) const override {}
		void take_screenshot(std::vector<u8>&&, u32, u32, bool) override {}
		void update_title(double) override {}
	};

	std::string exe_dir()
	{
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buf, MAX_PATH);
		return std::filesystem::path(buf).parent_path().string();
	}

	// Launcher graphics settings (settings.bat): applied over rpcs3_config.yml. Unset = yml value.
	void apply_launcher_overrides()
	{
		auto env = [](const char* n) -> const char* { const char* e = std::getenv(n); return e && *e ? e : nullptr; };
		// ---- R2PORT autoconfig: R2_PRESET=auto|low|medium|high|ultra (project/r2_autoconfig.h) ----
		// Without the launcher: fills the renderer vars below (RSX_RES_SCALE/FILTER/SHARPEN/ANISO/FSR/UPSCALE)
		// from the same mapping Launcher.exe uses. Only unset vars are filled, so explicit env vars always win.
		// R2_FPS, RSX_VSYNC, PORT_CPU_PROFILE and R2_SHADER_MODE are read before the renderer starts; they
		// need r2ac::apply_preset_env(getenv("R2_PRESET"), r2ac::Phase::startup) at the top of main().
		if (const char* p = env("R2_PRESET"))
		{
			char sum[512] = {};
			r2ac::apply_preset_env(p, r2ac::Phase::renderer, sum, sizeof sum);
			std::printf("[r2rsx] R2_PRESET=%s: %s\n", p, sum);
		}
		// ---- end R2PORT autoconfig ----
		if (const char* e = env("RSX_RES_SCALE"))
		{
			const double s = std::atof(e);
			if (s >= 0.25 && s <= 8.0) g_cfg.video.resolution_scale_percent.set(static_cast<u32>(s * 100.0 + 0.5));
		}
		if (const char* e = env("RSX_FILTER"))
		{
			if (!_stricmp(e, "point") || !_stricmp(e, "nearest")) g_cfg.video.output_scaling.set(output_scaling_mode::nearest);
			else if (!_stricmp(e, "fsr")) g_cfg.video.output_scaling.set(output_scaling_mode::fsr);
			else g_cfg.video.output_scaling.set(output_scaling_mode::bilinear);
		}
		if (const char* e = env("RSX_SHARPEN"))
		{
			const double s = std::atof(e);
			if (s >= 0.0 && s <= 1.0) g_cfg.video.rcas_sharpening_intensity.set(static_cast<u32>(s * 100.0 + 0.5));
		}
		if (const char* e = env("RSX_ANISO"))
		{
			const int a = std::atoi(e);
			if (a >= 0 && a <= 16) g_cfg.video.anisotropic_level_override.set(static_cast<u32>(a));
		}
		// FSR 3.1 / DLAA (RSX_FSR=1/2, VK/VKR2Upscaler.cpp) replace the game's 2x MSAA: it needs single-sample scene targets.
		if (const char* e = env("RSX_FSR"); e && std::atoi(e) > 0)
			g_cfg.video.antialiasing_level.set(msaa_level::none);
		// Host VSync (swapchain present mode): 0 off, 1 on, adaptive.
		if (const char* e = env("RSX_VSYNC"))
			g_cfg.video.vsync.set(!_stricmp(e, "adaptive") ? vsync_mode::adaptive : std::atoi(e) > 0 ? vsync_mode::full : vsync_mode::off);
		// R2_PRESENT names the Vulkan present mode directly (wins over RSX_VSYNC): immediate = tearing allowed
		// (VRR / G-Sync / FreeSync with an fps cap), mailbox = no tearing, no vsync wait (falls back to relaxed
		// FIFO), fifo = classic vsync.
		if (const char* e = env("R2_PRESENT"))
		{
			if (!_stricmp(e, "immediate") || !_stricmp(e, "tearing") || !_stricmp(e, "vrr")) g_cfg.video.vsync.set(vsync_mode::off);
			else if (!_stricmp(e, "mailbox")) g_cfg.video.vsync.set(vsync_mode::adaptive);
			else if (!_stricmp(e, "fifo") || !_stricmp(e, "vsync")) g_cfg.video.vsync.set(vsync_mode::full);
		}
		// RSX_DISPLAY=exclusive: the fullscreen window (rsx_null_backend.c) may take Vulkan exclusive fullscreen.
		if (const char* e = env("RSX_DISPLAY"))
			g_cfg.video.vk.exclusive_fullscreen_mode.set(!_stricmp(e, "exclusive") ? vk_exclusive_fs_mode::enable : vk_exclusive_fs_mode::disable);
		std::printf("[r2rsx] video: scale %u%%, output scaling %d, sharpen %u, aniso %u, vsync %d, exclusive fs %d\n",
			g_cfg.video.resolution_scale_percent.get(), static_cast<int>(g_cfg.video.output_scaling.get()),
			g_cfg.video.rcas_sharpening_intensity.get(), g_cfg.video.anisotropic_level_override.get(),
			static_cast<int>(g_cfg.video.vsync.get()), static_cast<int>(g_cfg.video.vk.exclusive_fullscreen_mode.get()));
	}

	// Shader compilation and logging defaults (env overrides, applied over rpcs3_config.yml).
	//  R2_SHADER_MODE = async (default: async recompiler, draws skipped until ready)
	//                 | interp (async recompiler + shader interpreter until ready; untested) | sync | interpreter
	//  R2_SHADER_CACHE=0 disables the on-disk shader cache (shadercache\rpcs3\shaders_cache).
	void apply_shader_defaults()
	{
		const char* m = std::getenv("R2_SHADER_MODE");
		shader_mode mode = shader_mode::async_recompiler;
		if (m && *m)
		{
			if (!_stricmp(m, "interp")) mode = shader_mode::async_with_interpreter;
			else if (!_stricmp(m, "sync") || !_stricmp(m, "legacy")) mode = shader_mode::recompiler;
			else if (!_stricmp(m, "interpreter")) mode = shader_mode::interpreter_only;
		}
		g_cfg.video.shadermode.set(mode);
		const char* c = std::getenv("R2_SHADER_CACHE");
		g_cfg.video.disable_on_disk_shader_cache.set(c && *c == '0');
		g_cfg.video.log_programs.set(false);
		std::printf("[r2rsx] shaders: mode %d, disk cache %s\n", static_cast<int>(mode),
			g_cfg.video.disable_on_disk_shader_cache.get() ? "off" : "on");
	}

	// ---- R2PORT perf (RPCS3 path speed / pacing / quality switches) ----
	const char* penv(const char* n) { const char* e = std::getenv(n); return e && *e ? e : nullptr; }

	// 1/0 for "1/on/true/yes" and "0/off/false/no"; -1 when unset or unrecognised.
	int penv_bool(const char* n)
	{
		const char* e = penv(n);
		if (!e) return -1;
		if (!_stricmp(e, "1") || !_stricmp(e, "on") || !_stricmp(e, "true") || !_stricmp(e, "yes")) return 1;
		if (!_stricmp(e, "0") || !_stricmp(e, "off") || !_stricmp(e, "false") || !_stricmp(e, "no")) return 0;
		return -1;
	}

	// Host CPU as project/cpu_profile.inc classifies it: PORT_CPU_PROFILE, else cores available to the
	// process (hybrid: P + E/2) <= 4 low, < 8 mid, else high.
	enum { cpu_off = 0, cpu_low, cpu_mid, cpu_high };
	struct host_cpu_t { int logical = 1; double eff_cores = 1; int profile = cpu_high; bool forced = false; };

	host_cpu_t detect_host_cpu()
	{
		host_cpu_t c;
		DWORD_PTR pm = 0, sm = 0;
		if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) pm = ~DWORD_PTR{0};
		c.logical = std::max(1, std::popcount(static_cast<unsigned long long>(pm)));
		c.logical = std::min<int>(c.logical, static_cast<int>(std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : c.logical));

		DWORD len = 0;
		GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
		std::vector<u8> buf(len);
		int cores = 0, pcores = 0, ecores = 0, max_class = 0;
		if (len && GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
		{
			for (int pass = 0; pass < 2; pass++)
			{
				for (DWORD o = 0; o < len;)
				{
					const auto* p = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + o);
					if (p->Relationship == RelationProcessorCore && p->Processor.GroupMask[0].Group == 0 &&
						(p->Processor.GroupMask[0].Mask & pm))
					{
						const int cls = p->Processor.EfficiencyClass;
						if (pass == 0) { max_class = std::max(max_class, cls); cores++; }
						else if (max_class > 0 && cls == 0) ecores++;
						else pcores++;
					}
					o += p->Size;
				}
			}
		}
		c.eff_cores = cores ? (max_class > 0 ? pcores + ecores / 2.0 : static_cast<double>(cores)) : c.logical;
		c.profile = c.eff_cores <= 4.0 ? cpu_low : c.eff_cores < 8.0 ? cpu_mid : cpu_high;
		if (const char* e = penv("PORT_CPU_PROFILE"); e && _stricmp(e, "auto"))
		{
			c.forced = true;
			if (!_stricmp(e, "low")) c.profile = cpu_low;
			else if (!_stricmp(e, "mid")) c.profile = cpu_mid;
			else if (!_stricmp(e, "high")) c.profile = cpu_high;
			else if (!_stricmp(e, "off") || !std::strcmp(e, "0")) c.profile = cpu_off;
			else c.forced = false;
		}
		return c;
	}

	// RPCS3's vblank / frame limit follow R2_FPS the way project/main.cpp sets them for the other
	// renderers (frame_clock): above 60 fps the vblank rate is the target, uncapped uses
	// max(display refresh, 240) Hz; the frame limit is one vblank ("Auto") up to 60 fps and off above,
	// where the game limiter (r2_frame_period) paces. RSX_VBLANK_US, RSX_CFG_LEGACY,
	// GCM_VBLANK_FOLLOW_FPS=0 and GCM_FRAME_LIMIT=off|vblank|<Hz> override as they do there.
	void apply_pacing()
	{
		const double fps = r2_fps_target();
		const char* vbe = penv("RSX_VBLANK_US");
		const char* leg = penv("RSX_CFG_LEGACY");
		const char* follow = penv("GCM_VBLANK_FOLLOW_FPS");
		double vb_hz = 0.; // 0: keep rpcs3_config.yml (Vblank Rate 60)
		if (vbe && std::atoll(vbe) >= 1000)
		{
			vb_hz = 1e6 / static_cast<double>(std::atoll(vbe));
		}
		else if (!(follow && *follow == '0'))
		{
			double hz = fps;
			if (fps <= 0.0) { hz = r2_display_refresh_hz(); if (hz < 240.0) hz = 240.0; }
			if (hz > 60.5) vb_hz = std::min(hz, 1000.0); // main.cpp: vblank period >= 1000 us
		}
		if (vb_hz > 0.)
		{
			g_cfg.video.vblank_rate.set(std::clamp<s64>(std::llround(vb_hz), 1, 6000));
			g_cfg.video.vblank_ntsc.set(false);
		}
		else if (leg && *leg != '0')
		{
			// 59.94 Hz: RPCS3 period (1'000'000 + 1000) / 60 = 16683 us, as RSX_CFG_LEGACY in main.cpp
			g_cfg.video.vblank_rate.set(60);
			g_cfg.video.vblank_ntsc.set(true);
		}

		const char* fl = penv("GCM_FRAME_LIMIT");
		if (fl && (!_stricmp(fl, "off") || !std::strcmp(fl, "0") || !_stricmp(fl, "none")))
		{
			g_cfg.video.frame_limit.set(frame_limit_type::none);
			g_cfg.video.second_frame_limit.set(0);
		}
		else if (fl && !_stricmp(fl, "vblank"))
		{
			g_cfg.video.frame_limit.set(frame_limit_type::_auto);
		}
		else if (fl && std::atof(fl) >= 1.0)
		{
			g_cfg.video.frame_limit.set(frame_limit_type::none);
			g_cfg.video.second_frame_limit.set(std::min(std::atof(fl), 1000.0));
		}
		else if (!(fps > 0.0 && fps <= 60.5))
		{
			g_cfg.video.frame_limit.set(frame_limit_type::none);
			g_cfg.video.second_frame_limit.set(0);
		}
		// else: up to 60 fps keep the yml limit (Auto = one vblank), main.cpp's min(1/fps, vblank)

		std::printf("[r2rsx] pacing: R2_FPS %.1f -> vblank %d Hz%s, frame limit %d, second limit %.1f\n",
			fps, static_cast<int>(g_cfg.video.vblank_rate.get()), g_cfg.video.vblank_ntsc.get() ? " (NTSC 59.94)" : "",
			static_cast<int>(g_cfg.video.frame_limit.get()), static_cast<double>(g_cfg.video.second_frame_limit.get()));
	}

	// Speed and quality switches over rpcs3_config.yml. Unset = yml value (no visual change by default).
	void apply_port_perf()
	{
		const host_cpu_t cpu = detect_host_cpu();
		static const char* const pn[] = { "off", "low", "mid", "high" };

		// Async pipeline compiler pool (draws with an uncompiled pipeline are skipped until it is ready).
		// RPCS3's auto: 1 below 8 threads, 2 at 8, 4/6/8/12 above. Port: low keeps cpu_profile.inc's
		// RSX_VK_PIPELINE_THREADS (1, or 2 with >= 8 threads); mid/high use the threads the game leaves
		// free (logical - 8, 2..4 mid, 2..12 high). R2_SHADER_THREADS=N overrides; profile off = RPCS3's.
		int sct = 0;
		if (const char* e = penv("R2_SHADER_THREADS")) sct = std::clamp(std::atoi(e), 0, 16);
		else if (cpu.profile == cpu_low)
		{
			const char* p = penv("RSX_VK_PIPELINE_THREADS");
			sct = p ? std::clamp(std::atoi(p), 1, 4) : (cpu.logical >= 8 ? 2 : 1);
		}
		else if (cpu.profile == cpu_mid) sct = std::clamp(cpu.logical - 8, 2, 4);
		else if (cpu.profile == cpu_high) sct = std::clamp(cpu.logical - 8, 2, 12);
		if (g_cfg.video.shader_compiler_threads_count.get() == 0)
			r2port::g_pipe_compiler_threads = sct;

		// Multithreaded RSX (the "RSX Offloader": guest->host copies off the RSX thread). yml: on.
		if (const int b = penv_bool("R2_MTRSX"); b >= 0) g_cfg.video.multithreaded_rsx.set(b != 0);

		// A/B switches that may change the image; defaults keep the yml.
		if (const int b = penv_bool("R2_WCB"); b >= 0) g_cfg.video.write_color_buffers.set(b != 0);
		if (const int b = penv_bool("R2_ZCULL_ACCURATE"); b >= 0) g_cfg.video.precise_zpass_count.set(b != 0);
		// R3PORT: relaxed by default, as the RPCS3 wiki recommends for Resistance 3. Occlusion results are written
		// once the GPU has them instead of stalling rsx::thread at every report sync point (bench r3a: 3 syncs and
		// 8.5 ms per frame); a report the game reads may be up to a frame old. The Z-pass counts themselves stay as
		// precise as R2_ZCULL_ACCURATE / the yml say. R2_ZCULL=sync restores the yml (strict) behaviour.
		if (const char* e = penv("R2_ZCULL"))
		{
			// relaxed: report reads do not wait on the GPU (results may lag a frame);
			// off: no occlusion queries at all (every query reports visible); sync/anything else: strict
			g_cfg.video.relaxed_zcull_sync.set(!_stricmp(e, "relaxed"));
			g_cfg.video.disable_zcull_queries.set(!_stricmp(e, "off"));
		}
		else
		{
			g_cfg.video.relaxed_zcull_sync.set(true);
		}
		if (const int b = penv_bool("R2_ASYNC_TEX"); b >= 0) g_cfg.video.vk.asynchronous_texture_streaming.set(b != 0);
		if (const int b = penv_bool("R2_HOST_LABELS"); b >= 0) g_cfg.video.host_label_synchronization.set(b != 0);

		// Quality (off/yml by default)
		if (const char* e = penv("R2_MSAA"); e && !(penv("RSX_FSR") && std::atoi(penv("RSX_FSR")) > 0))
			g_cfg.video.antialiasing_level.set(!_stricmp(e, "0") || !_stricmp(e, "off") || !_stricmp(e, "none") ? msaa_level::none : msaa_level::_auto);
		if (const int b = penv_bool("R2_HIGHP_Z"); b >= 0) g_cfg.video.force_high_precision_z_buffer.set(b != 0);
		if (const char* e = penv("R2_LOD_BIAS")) g_cfg.video.texture_lod_bias.set(std::clamp(std::atof(e), -32.0, 32.0));
		if (const int b = penv_bool("R2_GPU_TEX_SCALING"); b >= 0) g_cfg.video.use_gpu_texture_scaling.set(b != 0);

		apply_pacing();

		r2prof::enabled = penv_bool("R2_RSX_PROF") == 1 || s_prof_overlay;

		std::printf("[r2rsx] perf: cpu %s%s (%d threads, %.1f cores), shader compilers %d, MTRSX %d, async tex %d, "
			"WCB %d, ZCULL accurate %d relaxed %d off %d, host labels %d | MSAA %d, high-precision Z %d, LOD bias %.2f, "
			"GPU tex scaling %d | profiling %s\n",
			pn[cpu.profile], cpu.forced ? " (PORT_CPU_PROFILE)" : "", cpu.logical, cpu.eff_cores,
			r2port::g_pipe_compiler_threads ? r2port::g_pipe_compiler_threads : static_cast<int>(g_cfg.video.shader_compiler_threads_count.get()),
			g_cfg.video.multithreaded_rsx.get() ? 1 : 0, g_cfg.video.vk.asynchronous_texture_streaming.get() ? 1 : 0,
			g_cfg.video.write_color_buffers.get() ? 1 : 0, g_cfg.video.precise_zpass_count.get() ? 1 : 0,
			g_cfg.video.relaxed_zcull_sync.get() ? 1 : 0, g_cfg.video.disable_zcull_queries.get() ? 1 : 0,
			g_cfg.video.host_label_synchronization.get() ? 1 : 0,
			static_cast<int>(g_cfg.video.antialiasing_level.get()), g_cfg.video.force_high_precision_z_buffer.get() ? 1 : 0,
			static_cast<double>(g_cfg.video.texture_lod_bias.get()), g_cfg.video.use_gpu_texture_scaling.get() ? 1 : 0,
			r2prof::enabled ? "on (R2_RSX_PROF, every 5 s)" : "off");
	}

	// R2_RSX_PROF=1: every 5 s, one block of [r2rsx-prof] lines on stdout.
	struct thread_cpu_t { std::wstring name; u64 cpu100ns = 0; };

	// CPU time (100 ns units) of this process's named RSX threads, by thread id.
	// R2PORT round 7: the thread scan (system-wide toolhelp snapshot + open/describe every thread
	// of the process) cost the monitor ~0.25 s of CPU per 5 s sample in rp8 (thrcpu "r2rsx prof
	// monitor" 20-30% one window in five). The matching threads are cached with their handles and
	// the scan is redone only every 12th sample (1 min) or when a cached thread has exited.
	struct rsx_thread_ref { DWORD tid; HANDLE h; std::wstring name; };
	std::vector<rsx_thread_ref> s_rsx_threads;
	unsigned s_rsx_scan_age = ~0u;
	u64 s_mon_scan_ns = 0, s_mon_scans = 0, s_mon_last_work_ns = 0;

	void scan_rsx_threads()
	{
		const u64 t0 = r2prof::now_ns();
		for (auto& t : s_rsx_threads) CloseHandle(t.h);
		s_rsx_threads.clear();
		const DWORD pid = GetCurrentProcessId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snap != INVALID_HANDLE_VALUE)
		{
			THREADENTRY32 te{};
			te.dwSize = sizeof(te);
			for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
			{
				if (te.th32OwnerProcessID != pid) continue;
				HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
				if (!h) continue;
				PWSTR d = nullptr;
				if (SUCCEEDED(GetThreadDescription(h, &d)) && d && d[0] &&
					(wcsstr(d, L"rsx::thread") || wcsstr(d, L"RSX ") || wcsstr(d, L"RSX.") || wcsstr(d, L"VBlank")))
				{
					s_rsx_threads.push_back({ te.th32ThreadID, h, d });
					h = nullptr;
				}
				if (d) LocalFree(d);
				if (h) CloseHandle(h);
			}
			CloseHandle(snap);
		}
		s_rsx_scan_age = 0;
		s_mon_scan_ns += r2prof::now_ns() - t0;
		s_mon_scans++;
	}

	std::map<DWORD, thread_cpu_t> sample_rsx_threads()
	{
		if (++s_rsx_scan_age >= 12 || s_rsx_threads.empty()) scan_rsx_threads();
		std::map<DWORD, thread_cpu_t> out;
		bool exited = false;
		for (const auto& t : s_rsx_threads)
		{
			FILETIME c, e, k, u;
			DWORD code = 0;
			if (!GetExitCodeThread(t.h, &code) || code != STILL_ACTIVE) { exited = true; continue; }
			if (GetThreadTimes(t.h, &c, &e, &k, &u))
			{
				const u64 kk = (u64{k.dwHighDateTime} << 32) | k.dwLowDateTime;
				const u64 uu = (u64{u.dwHighDateTime} << 32) | u.dwLowDateTime;
				out[t.tid] = { t.name, kk + uu };
			}
		}
		if (exited) s_rsx_scan_age = ~0u - 1; // rescan on the next sample
		return out;
	}

	struct prof_snap_t
	{
		u64 frames, draws, submits, setup, program, vertex, texture, exec, flip, fb_ns, draw_ns;
		u64 vreq, vmiss, plook, pelide, flushes, hard, flush_ns, zr, zr_ns, av, av_h, av_ns, av_fl;
		u64 x[r2prof::x_count];
	};

	prof_snap_t prof_snap()
	{
		auto& g = r2prof::g;
		auto l = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
		prof_snap_t s{ l(g.frames), l(g.draws), l(g.submits), l(g.setup_us), l(g.program_us), l(g.vertex_us), l(g.texture_us),
			l(g.exec_us), l(g.flip_us), l(g.fb_ns), l(g.draw_ns), l(g.vtx_cache_req), l(g.vtx_cache_miss), l(g.prog_lookups),
			l(g.prog_elided), l(g.flushes), l(g.hard_syncs), l(g.flush_ns), l(g.zcull_reads), l(g.zcull_read_ns),
			l(g.av_calls), l(g.av_handled), l(g.av_ns), l(g.av_flushes), {} };
		for (int i = 0; i < r2prof::x_count; ++i)
			s.x[i] = (i == r2prof::av_max_ns || i == r2prof::fbb_lk_max_ns) ? g.x[i].exchange(0, std::memory_order_relaxed) : l(g.x[i]);
		return s;
	}

	void prof_thread_main()
	{
		SetThreadDescription(GetCurrentThread(), L"r2rsx prof monitor");
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // R2PORT round 7: never preempt game threads
		const auto self_cpu_ns = [] {
			FILETIME c, e, k, u;
			if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return u64{0};
			return ((u64{k.dwHighDateTime} << 32 | k.dwLowDateTime) + (u64{u.dwHighDateTime} << 32 | u.dwLowDateTime)) * 100;
		};
		prof_snap_t p0 = prof_snap();
		auto t0 = std::chrono::steady_clock::now();
		auto th0 = sample_rsx_threads();
		u64 idle0 = s_render ? s_render->performance_counters.idle_time.load() : 0;

		u64 mon_cpu0 = self_cpu_ns(), mon_scan0 = s_mon_scan_ns, mon_scans0 = s_mon_scans;
		// R2PORT round 7: the block is formatted into one buffer and written once, so the CRT
		// stdout lock is held for a single write instead of ~16 printf calls.
		std::string blk;
		const auto bp = [&blk](const char* fmt, auto... args) {
			char b[2048];
			const int n = std::snprintf(b, sizeof(b), fmt, args...);
			if (n > 0) blk.append(b, std::min<size_t>(static_cast<size_t>(n), sizeof(b) - 1));
		};
		for (;;)
		{
			Sleep(5000);
			const u64 work_t0 = r2prof::now_ns();
			const prof_snap_t p1 = prof_snap();
			const auto t1 = std::chrono::steady_clock::now();
			auto th1 = sample_rsx_threads();
			const u64 idle1 = s_render ? s_render->performance_counters.idle_time.load() : 0;

			const double wall_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
			const double secs = wall_us / 1e6;
			const double idle_us = static_cast<double>(idle1 >= idle0 ? idle1 - idle0 : idle1);
			const double frames = static_cast<double>(p1.frames - p0.frames);
			const double draws = static_cast<double>(p1.draws - p0.draws);
			const double fr = std::max(frames, 1.0), dr = std::max(draws, 1.0);

			// rsx::thread CPU and the other RSX threads
			double rsx_cpu_us = 0;
			std::map<std::wstring, double> others;
			for (const auto& [tid, t] : th1)
			{
				const auto it = th0.find(tid);
				const double us = static_cast<double>(t.cpu100ns - (it != th0.end() ? it->second.cpu100ns : 0)) / 10.0;
				std::wstring key = t.name;
				if (key.rfind(L"RSX.W", 0) == 0) key = L"RSX.W*";
				if (t.name == L"rsx::thread") rsx_cpu_us += us;
				else others[key] += us;
			}

			const double busy_us = std::max(0.0, wall_us - idle_us);
			const double ph_setup = static_cast<double>(p1.setup - p0.setup), ph_prog = static_cast<double>(p1.program - p0.program);
			const double ph_vtx = static_cast<double>(p1.vertex - p0.vertex), ph_tex = static_cast<double>(p1.texture - p0.texture);
			const double ph_exec = static_cast<double>(p1.exec - p0.exec), ph_flip = static_cast<double>(p1.flip - p0.flip);
			const double ph_fb = static_cast<double>(p1.fb_ns - p0.fb_ns) / 1000.0;
			const double draw_us = static_cast<double>(p1.draw_ns - p0.draw_ns) / 1000.0;
			const double in_draw_other = std::max(0.0, draw_us - (ph_fb + ph_setup + ph_prog + ph_vtx + ph_tex + ph_exec));
			const double non_draw = std::max(0.0, busy_us - draw_us - ph_flip);

			bp("[r2rsx-prof] %.1fs: %.1f fps, %.0f draws/f | rsx::thread cpu %.0f%%, busy %.0f%% (wall - FIFO/limiter idle) | "
				"busy %.2f us/draw (target < 6), cpu %.2f us/draw\n",
				secs, frames / secs, draws / fr, rsx_cpu_us * 100.0 / wall_us, busy_us * 100.0 / wall_us,
				busy_us / dr, rsx_cpu_us / dr);
			bp("[r2rsx-prof]   us/draw: draw %.2f = fb-bind %.2f + setup %.2f + program %.2f + textures %.2f + vertex %.2f + exec %.2f + other %.2f"
				" | outside draws %.2f (FIFO methods, flushes, ZCULL) | present %.2f ms/f\n",
				draw_us / dr, ph_fb / dr, ph_setup / dr, ph_prog / dr, ph_tex / dr, ph_vtx / dr, ph_exec / dr, in_draw_other / dr,
				non_draw / dr, ph_flip / 1000.0 / fr);
			const double vreq = static_cast<double>(p1.vreq - p0.vreq), plook = static_cast<double>(p1.plook - p0.plook);
			bp("[r2rsx-prof]   per frame: submits %.1f, flushes %.1f (hard %.2f, %.2f ms), ZCULL GPU reads %.1f (%.2f ms) | "
				"vertex cache miss %.0f%%, program lookups elided %.0f%%\n",
				static_cast<double>(p1.submits - p0.submits) / fr, static_cast<double>(p1.flushes - p0.flushes) / fr,
				static_cast<double>(p1.hard - p0.hard) / fr, static_cast<double>(p1.flush_ns - p0.flush_ns) / 1e6 / fr,
				static_cast<double>(p1.zr - p0.zr) / fr, static_cast<double>(p1.zr_ns - p0.zr_ns) / 1e6 / fr,
				vreq > 0 ? static_cast<double>(p1.vmiss - p0.vmiss) * 100.0 / vreq : 0.0,
				plook > 0 ? static_cast<double>(p1.pelide - p0.pelide) * 100.0 / plook : 0.0);
			const double av = static_cast<double>(p1.av - p0.av);
			std::string ths;
			for (const auto& [n, us] : others)
			{
				char b[96];
				std::snprintf(b, sizeof(b), "%s%ls %.0f%%", ths.empty() ? "" : ", ", n.c_str(), us * 100.0 / wall_us);
				ths += b;
			}
			bp("[r2rsx-prof]   page faults to RSX: %.0f/s, %.2f us each, %.2f ms/s, handled %.0f/s, GPU readbacks %.1f/s | threads: %s\n",
				av / secs, av > 0 ? static_cast<double>(p1.av_ns - p0.av_ns) / 1000.0 / av : 0.0,
				static_cast<double>(p1.av_ns - p0.av_ns) / 1e6 / secs, static_cast<double>(p1.av_h - p0.av_h) / secs,
				static_cast<double>(p1.av_fl - p0.av_fl) / secs, ths.empty() ? "-" : ths.c_str());

			// Round-2 breakdowns
			const auto dx = [&](r2prof::xc i) { return static_cast<double>(p1.x[i] - p0.x[i]); };
			const double fbb = std::max(dx(r2prof::fbb_full), 1.0);
			bp("[r2rsx-prof]   fb-bind: full rebinds %.1f/f (same-layout %.1f/f), us per full rebind %.1f = aa/dlss %.1f + surface cache %.1f + "
				"WCB copies %.1f + locks %.1f + renderpass/fbo %.1f | WCB copies %.1f/f, skipped (rebound) %.1f/f, DMA flushes in bind %.1f/f\n",
				dx(r2prof::fbb_full) / fr, dx(r2prof::fbb_ignore) / fr, dx(r2prof::fbb_total_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_aa_ns) / 1000.0 / fbb, dx(r2prof::fbb_prep_ns) / 1000.0 / fbb, dx(r2prof::fbb_wcb_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_lock_ns) / 1000.0 / fbb, dx(r2prof::fbb_rp_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_wcb_copies) / fr, dx(r2prof::fbb_wcb_skipped) / fr, dx(r2prof::fbb_dma_flush) / fr);
			const double zr = std::max(static_cast<double>(p1.zr - p0.zr), 1.0);
			const double avfl = std::max(static_cast<double>(p1.av_fl - p0.av_fl), 1.0);
			bp("[r2rsx-prof]   flushes by cause /f: zcull hint %.1f, zcull hard %.1f, fault request %.1f, bind DMA %.1f | "
				"ZCULL: syncs %.1f/f, %.1f reads/sync, batched reads %.1f/f (%.0f slots each), GPU poll loops %.1f/read, fault flushes served in wait %.1f/s\n",
				dx(r2prof::fl_zcull_hint) / fr, dx(r2prof::fl_zcull_hard) / fr, dx(r2prof::fl_av_request) / fr, dx(r2prof::fl_bind_dma) / fr,
				dx(r2prof::zc_sync_calls) / fr, static_cast<double>(p1.zr - p0.zr) / std::max(dx(r2prof::zc_sync_calls), 1.0),
				dx(r2prof::zc_batch_calls) / fr, dx(r2prof::zc_batch_slots) / std::max(dx(r2prof::zc_batch_calls), 1.0),
				dx(r2prof::zc_poll_loops) / zr, dx(r2prof::zc_flush_serviced) / secs);
			bp("[r2rsx-prof]   readback faults: wait for rsx::thread %.0f us + copy/GPU wait %.0f us avg, worst %.0f us | "
				"vertex uploads uncacheable %.0f%% | program slow-path loads %.1f/f\n",
				dx(r2prof::av_wait_ns) / 1000.0 / avfl, dx(r2prof::av_flush_ns) / 1000.0 / avfl, static_cast<double>(p1.x[r2prof::av_max_ns]) / 1000.0,
				vreq > 0 ? dx(r2prof::vtx_uncacheable) * 100.0 / vreq : 0.0, std::max(0.0, plook / 2.0 - dx(r2prof::prog_memo_hits)) / fr);

			// Round-3: outside draws, self times (nested scopes subtracted), ms per frame
			const double ms = 1e6 * fr;
			const double dlss = dx(r2prof::dlss_ns), dlss_draw = dx(r2prof::dlss_draw_ns);
			const double od_known = dx(r2prof::od_clear_ns) + dx(r2prof::od_fbb_ns) + dx(r2prof::od_zcull_ns) + dx(r2prof::od_flush_ns) +
				dx(r2prof::od_label_ns) + dx(r2prof::od_blit_ns) + dx(r2prof::od_flip_ns) + dx(r2prof::od_house_ns) +
				dx(r2prof::od_fl_drain_ns) + dx(r2prof::od_fl_reset_ns) + dx(r2prof::od_fl_end_ns) + dx(r2prof::od_fl_emu_ns); // R2PORT round 6: flip sub-buckets
			const double flip_all = dx(r2prof::od_flip_ns) + dx(r2prof::od_fl_drain_ns) + dx(r2prof::od_fl_reset_ns) + dx(r2prof::od_fl_end_ns) + dx(r2prof::od_fl_emu_ns);
			bp("[r2rsx-prof]   busy excluding DLSS/FSR hooks %.2f us/draw (hooks %.2f ms/f, %.2f of it inside draws) | outside draws ms/f: "
				"%.2f = clears %.2f (%.1f/f) + fb-bind %.2f (dlss/aa %.2f) + ZCULL wait %.2f + flushes %.2f + labels %.2f + blits %.2f + flip cmd %.2f + "
				"housekeeping %.2f + FIFO/methods %.2f\n",
				std::max(0.0, busy_us - dlss / 1000.0) / dr, dlss / ms, dlss_draw / ms,
				non_draw * 1000.0 / ms, dx(r2prof::od_clear_ns) / ms, dx(r2prof::od_clears) / fr,
				dx(r2prof::od_fbb_ns) / ms, std::max(0.0, dlss - dlss_draw) / ms, dx(r2prof::od_zcull_ns) / ms, dx(r2prof::od_flush_ns) / ms,
				dx(r2prof::od_label_ns) / ms, dx(r2prof::od_blit_ns) / ms, flip_all / ms, dx(r2prof::od_house_ns) / ms,
				std::max(0.0, non_draw * 1000.0 - od_known) / ms);
			bp("[r2rsx-prof]   ZCULL labels: deferred %.1f/f, released async %.1f/f, blocking drains %.1f/f (%.2f ms/f), fence sleeps %.1f/f | "
				"program: lookup %.2f us/draw, MRU hits %.1f/f, pipeline memo hits %.1f/f, re-sent FP kept %.1f/f, re-sent VP kept %.1f/f, dirty at draw /f: vp ucode %.1f, fp ucode/rehash %.1f, vp state-only %.1f, fp state-only %.1f "
				"(state-only keeps the hint unless R2_PROG_HINT=0)\n",
				dx(r2prof::zc_deferred) / fr, dx(r2prof::zc_released) / fr, dx(r2prof::zc_drain_block) / fr, dx(r2prof::zc_drain_ns) / ms,
				dx(r2prof::zc_fence_waits) / fr, dx(r2prof::prog_lookup_ns) / 1000.0 / dr, dx(r2prof::prog_mru_hits) / fr, dx(r2prof::prog_memo_hits) / fr, dx(r2prof::prog_fp_rebind) / fr, dx(r2prof::prog_vp_rebind) / fr,
				dx(r2prof::prog_vp_ucode) / fr, dx(r2prof::prog_fp_ucode) / fr, dx(r2prof::prog_vp_state) / fr, dx(r2prof::prog_fp_state) / fr);
			// Round-4: fb-bind lock phase, texture cache, VirtualProtect, flip, program split
			bp("[r2rsx-prof]   fb-bind locks us/rebind: discard %.1f + orphan %.1f + color %.1f + depth %.1f, worst %.0f us, slow (>20us) %.1f/f | "
				"keep (R2_FBB_KEEP) depth %.1f/f color %.1f/f\n",
				dx(r2prof::fbb_lk_discard_ns) / 1000.0 / fbb, dx(r2prof::fbb_lk_orphan_ns) / 1000.0 / fbb,
				dx(r2prof::fbb_lk_color_ns) / 1000.0 / fbb, dx(r2prof::fbb_lk_depth_ns) / 1000.0 / fbb,
				static_cast<double>(p1.x[r2prof::fbb_lk_max_ns]) / 1000.0, dx(r2prof::fbb_lk_slow) / fr,
				dx(r2prof::fbb_keep_depth) / fr, dx(r2prof::fbb_keep_color) / fr);
			const double vpc = std::max(dx(r2prof::mm_vp_calls), 1.0);
			bp("[r2rsx-prof]   texture cache: mutex wait %.2f ms/f, invalidations %.1f/f (%.2f ms/f), sections unprotected %.1f/f, relocks %.1f/f, fresh locks %.1f/f | "
				"VirtualProtect: %.1f/f, %.2f ms/f (%.1f us each, %.1f pages each), in fb-bind %.1f/f (%.2f ms/f), skipped same-prot (R2_MM_SKIPSAME) %.1f/f | "
				"mm queue lock wait %.2f ms/f, Offloader mm flushes %.1f/f\n",
				dx(r2prof::tc_mutex_wait_ns) / ms, dx(r2prof::tc_inval_calls) / fr, dx(r2prof::tc_inval_ns) / ms, dx(r2prof::tc_unprotected) / fr,
				dx(r2prof::tc_relock) / fr, dx(r2prof::tc_fresh_lock) / fr,
				dx(r2prof::mm_vp_calls) / fr, dx(r2prof::mm_vp_ns) / ms, dx(r2prof::mm_vp_ns) / 1000.0 / vpc, dx(r2prof::mm_vp_pages) / vpc,
				dx(r2prof::mm_vp_fbb_calls) / fr, dx(r2prof::mm_vp_fbb_ns) / ms, dx(r2prof::mm_vp_skipped) / fr,
				dx(r2prof::mm_qlock_wait_ns) / ms, dx(r2prof::mm_offload_flushes) / fr);
			const double xf_ns = dx(r2prof::prog_xform_ns), fc_ns = dx(r2prof::prog_fconst_ns), env_ns = dx(r2prof::prog_env_ns);
			bp("[r2rsx-prof]   flip: frame limiter sleep %.2f ms/f (idle), present %.2f ms/f, ZCULL labels carried over flip %.1f/f (R2_ZCULL_FLIP_LAG) | "
				"program us/draw: analyse %.2f, load_program %.2f, env %.2f = xform consts %.2f (%.0f B/draw) + frag consts %.2f + rest %.2f\n",
				dx(r2prof::od_limit_ns) / ms, dx(r2prof::od_present_ns) / ms, dx(r2prof::zc_flip_carried) / fr,
				dx(r2prof::prog_analyse_ns) / 1000.0 / dr, dx(r2prof::prog_load_ns) / 1000.0 / dr, env_ns / 1000.0 / dr,
				xf_ns / 1000.0 / dr, dx(r2prof::prog_xform_bytes) / dr, fc_ns / 1000.0 / dr, std::max(0.0, env_ns - xf_ns - fc_ns) / 1000.0 / dr);

			// Round-5: texture-cache readbacks inside invalidations, program env "rest" split
			const double tfc = std::max(dx(r2prof::tc_flush_calls), 1.0), tfcc = std::max(dx(r2prof::tc_flush_commit_calls), 1.0);
			bp("[r2rsx-prof]   texture cache readbacks (flush_set): %.1f/f, %.2f ms/f (%.0f us each, %.1f sections each), of which render-target commits %.1f/f, "
				"%.2f ms/f (%.0f us each) | commit flushes skipped (R2_FBO_COMMIT_FLUSH=0 default) %.1f sections/f\n",
				dx(r2prof::tc_flush_calls) / fr, dx(r2prof::tc_flush_ns) / ms, dx(r2prof::tc_flush_ns) / 1000.0 / tfc, dx(r2prof::tc_flush_sections) / tfc,
				dx(r2prof::tc_flush_commit_calls) / fr, dx(r2prof::tc_flush_commit_ns) / ms, dx(r2prof::tc_flush_commit_ns) / 1000.0 / tfcc,
				dx(r2prof::tc_commit_skip) / fr);
			const double pe_known = dx(r2prof::pe_aa_ns) + dx(r2prof::pe_vtx_env_ns) + dx(r2prof::pe_inst_ns) + dx(r2prof::pe_frag_env_ns) +
				dx(r2prof::pe_tex_env_ns) + dx(r2prof::pe_bind_ns);
			bp("[r2rsx-prof]   program env rest us/draw: DLSS/FSR draw hook (r2aa_on_draw, incl. AA runs fired there) %.2f + vertex env %.2f + instancing %.2f + "
				"fragment env %.2f + texture env %.2f + bind_uniform %.2f + unaccounted %.2f\n",
				dx(r2prof::pe_aa_ns) / 1000.0 / dr, dx(r2prof::pe_vtx_env_ns) / 1000.0 / dr, dx(r2prof::pe_inst_ns) / 1000.0 / dr,
				dx(r2prof::pe_frag_env_ns) / 1000.0 / dr, dx(r2prof::pe_tex_env_ns) / 1000.0 / dr, dx(r2prof::pe_bind_ns) / 1000.0 / dr,
				std::max(0.0, env_ns - xf_ns - fc_ns - pe_known) / 1000.0 / dr);
			// Round-6: flip cmd split (self ms/f; limiter and present are listed on the flip: line), ZCULL reads by wait
			const double zrw = std::max(dx(r2prof::zc_read_wait), 1.0);
			bp("[r2rsx-prof]   flip cmd ms/f: %.2f = label drain/ZCULL poll %.2f + reset (ZCULL state) %.2f + frame end %.2f + emu flip %.2f + rest %.2f | "
				"ZCULL reads /f: no-wait %.1f, waited %.1f (%.2f ms/f, %.0f us each), by update %.1f, forced %.1f (R2_ZCULL_NOWAIT)\n",
				flip_all / ms, dx(r2prof::od_fl_drain_ns) / ms, dx(r2prof::od_fl_reset_ns) / ms, dx(r2prof::od_fl_end_ns) / ms,
				dx(r2prof::od_fl_emu_ns) / ms, dx(r2prof::od_flip_ns) / ms,
				dx(r2prof::zc_read_ready) / fr, dx(r2prof::zc_read_wait) / fr, dx(r2prof::zc_read_wait_ns) / ms,
				dx(r2prof::zc_read_wait_ns) / 1000.0 / zrw, dx(r2prof::zc_read_update) / fr, dx(r2prof::zc_read_forced) / fr);
			// Round-7: draw window split and lock-phase internals
			{
				const double drw = std::max(dr, 1.0);
				const double ph_sum_ns = (ph_setup + ph_prog + ph_vtx + ph_tex + ph_exec) * 1000.0;
				const double b_ns = dx(r2prof::dr_begin_ns), e_ns = dx(r2prof::dr_end_ns);
				const double win_ns = draw_us * 1000.0;
				bp("[r2rsx-prof]   draw window us/draw: begin %.2f (fb-bind %.2f + cond-render/other %.2f), end %.2f (phases %.2f + other %.2f), "
					"methods between begin/end %.2f, post-draw tail (outside window) %.2f | "
					"lock: inval superseded %.1f us/rebind, create+reprotect %.1f us/rebind, fresh reasons /f: new %.1f, unlocked %.1f, ctx change %.1f; "
					"superseded surfaces %.1f/f | invalidate ms/f: intersect %.2f, unprotect %.2f, fast discards %.1f/f\n",
					b_ns / 1000.0 / drw, ph_fb / drw, std::max(0.0, b_ns / 1000.0 - ph_fb) / drw,
					e_ns / 1000.0 / drw, ph_sum_ns / 1000.0 / drw, std::max(0.0, e_ns - ph_sum_ns) / 1000.0 / drw,
					std::max(0.0, win_ns - b_ns - e_ns) / 1000.0 / drw, dx(r2prof::dr_post_ns) / 1000.0 / drw,
					dx(r2prof::lk_inval_ns) / 1000.0 / fbb, dx(r2prof::lk_create_ns) / 1000.0 / fbb,
					dx(r2prof::lk_fresh_noexist) / fr, dx(r2prof::lk_fresh_unlocked) / fr, dx(r2prof::lk_fresh_ctx) / fr,
					dx(r2prof::lk_superseded) / fr, dx(r2prof::inval_isect_ns) / ms, dx(r2prof::inval_unprot_ns) / ms, dx(r2prof::inval_discards) / fr);
			}
			// Round-7: the monitor's own cost (previous cycle's print included)
			const u64 mon_cpu1 = self_cpu_ns();
			bp("[r2rsx-prof]   monitor: cpu %.1f ms/5s (thread scans %llu, %.1f ms), last print %.1f ms wall\n",
				static_cast<double>(mon_cpu1 - mon_cpu0) / 1e6, static_cast<unsigned long long>(s_mon_scans - mon_scans0),
				static_cast<double>(s_mon_scan_ns - mon_scan0) / 1e6, static_cast<double>(s_mon_last_work_ns) / 1e6);
			mon_cpu0 = mon_cpu1; mon_scan0 = s_mon_scan_ns; mon_scans0 = s_mon_scans;
			std::fwrite(blk.data(), 1, blk.size(), stdout); // R2PORT round 7: one write per block
			std::fflush(stdout);
			blk.clear();
			s_mon_last_work_ns = r2prof::now_ns() - work_t0;

			p0 = p1; t0 = t1; th0 = std::move(th1); idle0 = idle1;
		}
	}
	// ---- end R2PORT perf ----

	void load_config()
	{
		// R2_RSX_CONFIG, else rpcs3_config.yml next to the exe or in its parent: an RPCS3 config.yml.
		std::vector<std::string> paths;
		if (const char* e = std::getenv("R2_RSX_CONFIG"); e && *e) paths.emplace_back(e);
		const std::string dir = exe_dir();
		paths.push_back(dir + "\\rpcs3_config.yml");
		paths.push_back(dir + "\\..\\rpcs3_config.yml");

		for (const auto& p : paths)
		{
			std::ifstream f(p, std::ios::binary);
			if (!f) continue;
			// Only the top-level Video: block. The other sections name enums whose
			// formatters live in parts of RPCS3 that are not vendored, and parsing
			// those never terminates (cfg::try_to_enum_value).
			std::string line, video;
			bool in_video = false;
			while (std::getline(f, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (!line.empty() && line[0] != ' ' && line[0] != '#')
					in_video = line.rfind("Video:", 0) == 0;
				if (in_video) video += line + '\n';
			}
			if (video.empty()) continue;
			if (g_cfg.from_string(video))
			{
				std::printf("[r2rsx] config: %s\n", p.c_str());
				break;
			}
			std::printf("[r2rsx] config: failed to parse %s\n", p.c_str());
		}

		g_cfg.video.renderer.set(video_renderer::vulkan);
		g_cfg.misc.use_native_interface.set(false);
		apply_shader_defaults();
		apply_launcher_overrides();
		apply_port_perf(); // R2PORT perf
	}

	void commit(u32 ea, u32 size)
	{
		for (u8* base : { vm_base, vm_sudo_base })
		{
			if (base) VirtualAlloc(base + ea, size, MEM_COMMIT, PAGE_READWRITE);
		}
	}

	int access_violation_hook(uint32_t addr, int is_write)
	{
		if (!r2prof::enabled) [[likely]]
		{
			return rsx::g_access_violation_handler && rsx::g_access_violation_handler(addr, is_write != 0);
		}

		// R2PORT perf: R2_RSX_PROF counts the faults and the time RPCS3 spends in them
		const u64 t0 = r2prof::now_ns();
		const int r = rsx::g_access_violation_handler && rsx::g_access_violation_handler(addr, is_write != 0);
		r2prof::add(r2prof::g.av_ns, r2prof::now_ns() - t0);
		r2prof::add(r2prof::g.av_calls, 1);
		if (r) r2prof::add(r2prof::g.av_handled, 1);
		return r;
	}
}

extern "C" void r2port_rsx_guest_call(u32 opd, u32 arg)
{
	{
		std::lock_guard lock(s_intr_mtx);
		s_intr_queue.emplace_back(opd, arg);
	}
	s_intr_cv.notify_one();
}

// R3PORT PC features: the F3 performance overlay (project/r3_pcfeat.cpp) turns the r2prof
// counters on while it is shown and samples them. R2_RSX_PROF keeps them on regardless.
extern "C" void r2rsx_prof_enable(int on)
{
	s_prof_overlay = on != 0;
	r2prof::enabled = s_prof_overlay || penv_bool("R2_RSX_PROF") == 1;
}

// Deltas since the previous call: RSX frames, draws per frame and average draw time (us,
// begin() .. end of draw on the RSX thread). Returns 0 when there was no frame in between.
extern "C" int r2rsx_prof_sample(double* frames, double* draws_per_frame, double* us_per_draw)
{
	static uint64_t lf = 0, ld = 0, ln = 0;
	const uint64_t f = r2prof::g.frames.load(std::memory_order_relaxed);
	const uint64_t d = r2prof::g.draws.load(std::memory_order_relaxed);
	const uint64_t n = r2prof::g.draw_ns.load(std::memory_order_relaxed);
	const uint64_t df = f - lf, dd = d - ld, dn = n - ln;
	lf = f; ld = d; ln = n;
	if (frames) *frames = static_cast<double>(df);
	if (draws_per_frame) *draws_per_frame = df ? static_cast<double>(dd) / df : 0.0;
	if (us_per_draw) *us_per_draw = dd ? static_cast<double>(dn) / dd / 1000.0 : 0.0;
	return df != 0;
}

extern "C" int r2rsx_enabled(void)
{
	if (s_enabled < 0)
	{
		// RPCS3/Vulkan is the default renderer; R2_RSX=d3d12 (or 0) selects the old D3D12 backend,
		// R2_RSX=vulkan the native Vulkan renderer (ps3recomp's draw engine).
		const char* e = std::getenv("R2_RSX");
		s_enabled = !(e && (!_stricmp(e, "d3d12") || !_stricmp(e, "vulkan") || !_stricmp(e, "native") ||
		                    !std::strcmp(e, "0")));
	}
	return s_enabled;
}

extern "C" void r2rsx_set_window(void* hwnd, uint32_t, uint32_t)
{
	{
		std::lock_guard lock(s_win_mtx);
		s_hwnd = static_cast<HWND>(hwnd);
	}
	s_win_cv.notify_all();
}

extern "C" int r2rsx_context_allocate(uint32_t dma_control_ea, uint32_t label_ea,
                                      uint32_t driver_info_ea, uint32_t device_ea,
                                      uint32_t local_size)
{
	if (!r2rsx_enabled()) return -1;
	if (s_up) return 0;

	{
		std::unique_lock lock(s_win_mtx);
		if (!s_win_cv.wait_for(lock, std::chrono::seconds(30), [] { return s_hwnd != nullptr; }))
		{
			std::printf("[r2rsx] no window after 30 s\n");
			return -1;
		}
	}

	const std::string root = exe_dir() + "\\..";
	std::error_code ec;
	std::filesystem::create_directories(root + "\\logs", ec);
	// Keep the previous run's RSX log (a fatal rsx::thread error is only in there)
	if (std::filesystem::file_size(root + "\\logs\\rpcs3_rsx.log", ec) > 0 && !ec)
	{
		std::filesystem::rename(root + "\\logs\\rpcs3_rsx.log", root + "\\logs\\rpcs3_rsx.prev.log", ec);
	}
	ec.clear();
	s_log = logs::make_file_listener(root + "\\logs\\rpcs3_rsx.log", 256ull << 20);
	{
		// RSX log verbosity: errors only unless R2_DIAG=1 (RPCS3's notice level) or R2_RSX_LOG=<level>.
		const char* d = std::getenv("R2_DIAG");
		logs::level lv = d && *d && *d != '0' ? logs::level::notice : logs::level::error;
		if (const char* e = std::getenv("R2_RSX_LOG"); e && *e)
		{
			if (!_stricmp(e, "fatal")) lv = logs::level::fatal;
			else if (!_stricmp(e, "error")) lv = logs::level::error;
			else if (!_stricmp(e, "warning")) lv = logs::level::warning;
			else if (!_stricmp(e, "notice")) lv = logs::level::notice;
			else if (!_stricmp(e, "trace")) lv = logs::level::trace;
		}
		logs::set_level(".*", lv);
	}

	// Shader cache next to the other one in shadercache\ (run.bat backs that folder up).
	r2port::g_rsx_cache_dir = root + "\\shadercache\\rpcs3\\";
	if (!std::filesystem::exists(r2port::g_rsx_cache_dir, ec) && std::filesystem::exists(root + "\\cache\\rpcs3\\shaders_cache", ec))
	{
		std::filesystem::create_directories(root + "\\shadercache", ec);
		std::filesystem::rename(root + "\\cache\\rpcs3", r2port::g_rsx_cache_dir.substr(0, r2port::g_rsx_cache_dir.size() - 1), ec);
	}
	std::filesystem::create_directories(r2port::g_rsx_cache_dir, ec);

	load_config();

	g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return std::make_unique<r2_gs_frame>(); };
	// Headless: RPCS3 UI hooks must exist (empty std::function throws bad_function_call)
	g_emu_callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return nullptr; };
	g_emu_callbacks.get_localized_string = [](auto, const char* s) -> std::string { return s ? s : ""; };
	g_emu_callbacks.get_localized_u32string = [](auto, const char*) -> std::u32string { return {}; };
	g_emu_callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
	g_emu_callbacks.call_from_main_thread = [](std::function<void()> f, atomic_t<u32>* wake) { if (f) f(); if (wake) { *wake = 1; wake->notify_one(); } };

	// Local memory, the RsxReports block, driver info and the device page.
	commit(rsx::constants::local_mem_base, local_size);
	commit(label_ea & ~0xFFFu, 0x10000);
	commit(driver_info_ea & ~0xFFFu, 0x2000);
	commit(device_ea & ~0xFFFu, 0x1000);
	commit(dma_control_ea & ~0xFFFu, 0x1000);

	g_fxo->init();
	rsx::create_render_thread(nullptr);

	s_render = rsx::get_current_renderer();
	if (!s_render)
	{
		std::printf("[r2rsx] render thread was not created\n");
		return -1;
	}

	g_vm_access_violation_hook = access_violation_hook;

	auto* render = s_render;
	render->isHLE = true;
	render->local_mem_size = local_size;
	render->device_addr = device_ea;

	// sys_rsx_context_allocate
	auto& reports = *vm::_ptr<RsxReports>(label_ea);
	std::memset(&reports, 0, sizeof(RsxReports));
	for (auto& n : reports.notify) n.timestamp = -1;
	for (usz i = 0; i < std::size(reports.semaphore); i += 4)
	{
		reports.semaphore[i + 0] = 0x1337C0D3;
		reports.semaphore[i + 1] = 0x1337BABE;
		reports.semaphore[i + 2] = 0x1337BEEF;
		reports.semaphore[i + 3] = 0x1337F001;
	}
	for (auto& r : reports.report)
	{
		r.val = 0;
		r.timestamp = -1;
		r.pad = -1;
	}

	auto& driver = *vm::_ptr<RsxDriverInfo>(driver_info_ea);
	std::memset(&driver, 0, sizeof(RsxDriverInfo));
	driver.version_driver = 0x211;
	driver.version_gpu = 0x5c;
	driver.memory_size = local_size;
	driver.nvcore_frequency = 500000000;
	driver.memory_frequency = 650000000;
	driver.reportsNotifyOffset = 0x1000;
	driver.reportsOffset = 0;
	driver.reportsReportOffset = 0x1400;
	driver.hardware_channel = 1;

	auto& dma = *vm::_ptr<RsxDmaControl>(dma_control_ea);
	dma.get = 0;
	dma.put = 0;
	dma.ref = 0;

	rsx::context* ctx = render->get_context();
	ctx->fw.driver_info = driver_info_ea;
	ctx->fw.main_mem_size = 0x10000000;
	ctx->fw.label_addr = label_ea;
	ctx->display_buffers_count = 0;
	ctx->current_display_buffer = 0;

	render->init_context(ctx, dma_control_ea);

	// Vulkan device and backend resources
	while (!render->is_initialized)
	{
		Sleep(5);
	}

	std::thread(intr_thread_main).detach();
	if (r2prof::enabled) std::thread(prof_thread_main).detach(); // R2PORT perf

	render->start_device();
	s_up = true;

	std::printf("[r2rsx] RPCS3 RSX up (dma=0x%08X labels=0x%08X driver=0x%08X device=0x%08X local=0x%X)\n",
		dma_control_ea, label_ea, driver_info_ea, device_ea, local_size);
	return 0;
}

extern "C" void r2rsx_iomap(uint32_t ea, uint32_t io, uint32_t size)
{
	if (!s_up) return;
	auto& table = s_render->iomap_table;
	std::lock_guard lock(s_render->sys_rsx_mtx);

	ea >>= 20, io >>= 20, size >>= 20;
	for (u32 i = 0; i < size; i++)
	{
		const u32 prev_ea = table.ea[io + i];
		table.ea[io + i].release((ea + i) << 20);
		if (prev_ea + 1) table.io[prev_ea >> 20].release(-1);
		table.io[ea + i].release((io + i) << 20);
	}
}

extern "C" void r2rsx_iounmap(uint32_t io, uint32_t size)
{
	if (!s_up) return;
	auto& table = s_render->iomap_table;
	std::lock_guard lock(s_render->sys_rsx_mtx);

	for (const u32 end = (io >>= 20) + (size >>= 20); io < end;)
	{
		const u32 ea_entry = table.ea[io];
		table.ea[io++].release(-1);
		if (ea_entry + 1) table.io[ea_entry >> 20].release(-1);
	}
}

extern "C" void r2rsx_set_display_buffer(uint32_t id, uint32_t offset, uint32_t pitch,
                                         uint32_t width, uint32_t height)
{
	if (!s_up || id >= 8) return;
	rsx::context* ctx = s_render->get_context();
	auto& buf = ctx->display_buffers[id];
	buf.offset = offset;
	buf.pitch = pitch;
	buf.width = width;
	buf.height = height;
	if (ctx->display_buffers_count < id + 1) ctx->display_buffers_count = id + 1;
}

extern "C" void r2rsx_set_flip_mode(uint32_t mode)
{
	if (s_up) s_render->requested_vsync = (mode == 1);
}

extern "C" uint32_t r2rsx_get_flip_status(void)
{
	return s_up ? s_render->display_ctrl().get_flip_status() : 0;
}

extern "C" void r2rsx_set_flip_status(uint32_t status)
{
	if (s_up) s_render->display_ctrl().set_flip_status(status);
}

extern "C" uint64_t r2rsx_last_flip_time(void)
{
	return s_up ? s_render->display_ctrl().get_last_guest_flip_timestamp() : 0;
}

extern "C" uint64_t r2rsx_vblank_count(void)
{
	return s_up ? s_render->display_ctrl().get_vblank_count() : 0;
}

extern "C" uint32_t r2rsx_current_display_buffer(void)
{
	return s_up ? s_render->get_context()->current_display_buffer : 0;
}

extern "C" void r2rsx_set_flip_handler(uint32_t opd)   { if (s_up) s_render->flip_handler.set(opd); }
extern "C" void r2rsx_set_vblank_handler(uint32_t opd) { if (s_up) s_render->vblank_handler.set(opd); }
extern "C" void r2rsx_set_user_handler(uint32_t opd)   { if (s_up) s_render->user_handler.set(opd); }
extern "C" void r2rsx_set_queue_handler(uint32_t opd)  { if (s_up) s_render->queue_handler.set(opd); }

extern "C" void r2rsx_set_tile(uint32_t index, uint32_t location, uint32_t offset, uint32_t size,
                               uint32_t pitch, uint32_t comp, uint32_t base, uint32_t bank, int bound)
{
	if (!s_up || index >= std::size(s_render->tiles)) return;
	auto& tile = s_render->tiles[index];

	if (tile.bound && !bound)
	{
		s_render->notify_tile_unbound(index);
	}

	tile.location = location;
	tile.offset = offset;
	tile.size = size;
	tile.pitch = pitch;
	tile.comp = comp;
	tile.base = base;
	tile.bank = bank;
	tile.bound = bound != 0;
}

extern "C" void r2rsx_set_zcull(uint32_t index, uint32_t offset, uint32_t width, uint32_t height,
                                uint32_t cullStart, uint32_t zFormat, uint32_t aaFormat,
                                uint32_t zcullDir, uint32_t zcullFormat, uint32_t sFunc,
                                uint32_t sRef, uint32_t sMask, int bound)
{
	if (!s_up || index >= std::size(s_render->zculls)) return;
	auto& z = s_render->zculls[index];
	z.offset = offset;
	z.width = width;
	z.height = height;
	z.cullStart = cullStart;
	z.zFormat = zFormat;
	z.aaFormat = aaFormat;
	z.zcullDir = zcullDir;
	z.zcullFormat = zcullFormat;
	z.sFunc = sFunc;
	z.sRef = sRef;
	z.sMask = sMask;
	z.bound = bound != 0;
}

extern "C" void r2rsx_wait_get(uint32_t put, unsigned max_ms)
{
	if (!s_up || !s_render->ctrl) return;
	const u64 deadline = GetTickCount64() + max_ms;
	while (s_render->ctrl->get != put && GetTickCount64() < deadline)
	{
		Sleep(0);
	}
}
