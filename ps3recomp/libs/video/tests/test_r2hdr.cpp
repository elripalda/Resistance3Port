// VKR2Hdr.cpp: HDR settings (port prefixes, PORT_ aliases, clamps, auto) and the present-pass GLSL entry.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/VK/VKR2Hdr.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main()
{
	// the port's own names win over PORT_; values are clamped
	_putenv_s("PORT_HDR", "0");
	_putenv_s("R3_HDR", "auto");
	_putenv_s("PORT_HDR_PAPER", "50");     // below 80 -> 80
	_putenv_s("PORT_HDR_PEAK", "60");      // below paper -> paper (no expansion)
	_putenv_s("PORT_HDR_SAT", "0.5");
	_putenv_s("R2_HDR_SAT", "5");          // above 2 -> 2
	CHECK(r2hdr::request() == 1, "auto asks for HDR10, got %d", r2hdr::request());
	CHECK(r2hdr::is_auto(), "auto");
	CHECK(r2hdr::paper() == 80.f, "paper clamp %f", r2hdr::paper());
	CHECK(r2hdr::peak() == 80.f, "peak >= paper %f", r2hdr::peak());
	CHECK(!std::strcmp(r2hdr::peak_source(), "setting"), "peak source %s", r2hdr::peak_source());
	CHECK(r2hdr::sat() == 2.f, "sat clamp / prefix order %f", r2hdr::sat());
	const int d = r2hdr::display_hdr();
	CHECK(d >= -1 && d <= 1, "display state %d", d);
	CHECK(r2hdr::allowed() == (d == 1), "auto needs Windows HDR on (display %d, allowed %d)", d, r2hdr::allowed());
	const char* g = r2hdr::glsl();
	CHECK(std::strstr(g, "vec3 r2pc_hdr(vec3 c, vec2 frag_xy, int mode, float paper, float peak, float sat)") != nullptr, "GLSL entry");
	CHECK(std::strstr(g, "const float kn = 0.6;") != nullptr, "GLSL knee = port_hdr.h PORT_HDR_KNEE");
	std::printf("display HDR state %d\n", d);
	if (!fails) std::printf("r2hdr: all checks passed\n");
	return fails != 0;
}
