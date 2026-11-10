/* RPCS3 texture/sampler parity rules in the production draw engine
 * (scratch/parity/textures.md); synthetic, no title data. */
#define main old_engine_tests_main
#include "test_rsx_draw_engine.c"
#undef main

static rsx_be_sampler_desc g_smp0;
static u32 g_created_w, g_created_h;

static void cap_bind_textures(void* u, const u32* tex, const rsx_be_sampler_desc* smp, u32 mask)
{
    if (mask & 1u) g_smp0 = smp[0];
    stub_bind_textures(u, tex, smp, mask);
}
static u32 cap_texture_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips, u32 faces,
                              u32 remap, u32 rsx_fmt)
{
    g_created_w = w; g_created_h = h;
    return stub_texture_create(u, f, w, h, mips, faces, remap, rsx_fmt);
}

/* Unit 0, swizzled 4x4 in local memory unless told otherwise. */
static void unit0(u32 offset, u32 fmt, u32 dim, u32 wrap, u32 remap, u32 filter, u32 border,
                  u32 rect)
{
    m(M_TEX_UNIT + 0x00, offset);
    m(M_TEX_UNIT + 0x04, 1u | (dim << 4) | (fmt << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x08, wrap);
    m(M_TEX_UNIT + 0x0C, 0x80000000u);
    m(M_TEX_UNIT + 0x10, remap);
    m(M_TEX_UNIT + 0x14, filter);
    m(M_TEX_UNIT + 0x18, rect);
    m(M_TEX_UNIT + 0x1C, border);
}

#define WRAP(s, t, r) ((s) | ((t) << 8) | ((r) << 16))
#define RECT4 ((4u << 16) | 4u)

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    if (!vm_base) return 2;
    ppu_vm_size = GUEST_BYTES;
    rsx_draw_backend be = g_stub_backend;
    be.bind_textures = cap_bind_textures;
    be.texture_create = cap_texture_create;
    g_stub_be_override = &be;
    engine_up();

    printf("-- convolution filters (VKFormats.cpp get_min_filter/get_mag_filter)\n");
    unit0(0x100000, 0x85, 2, WRAP(1, 1, 1), 0xAAE4, (7u << 16) | (4u << 24), 0, RECT4);
    draw_triangle();
    CHECK(g_smp0.min_linear && g_smp0.mag_linear && !g_smp0.mip_present,
          "CONVOLUTION_MIN/MAG sample LINEAR with no mips (%u %u %u)",
          g_smp0.min_linear, g_smp0.mag_linear, g_smp0.mip_present);

    printf("-- border colour (is_border_clamped_texture, border_color, remap)\n");
    const u32 lin = (2u << 16) | (2u << 24);
    unit0(0x100000, 0x85, 2, WRAP(3, 4, 1), 0xAAE4, lin, 0x80FF0000u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0x80FF0000u, "2D T border keeps the register (0x%08X)",
          g_smp0.border_color);
    unit0(0x100000, 0x85, 2, WRAP(3, 3, 4), 0xAAE4, lin, 0x80FF0000u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0xFF000000u,
          "2D with only R border is not border-clamped: opaque black (0x%08X)",
          g_smp0.border_color);
    unit0(0x100000, 0x85, 3, WRAP(3, 3, 4), 0xAAE4, lin, 0x80FF0000u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0x80FF0000u, "3D R border is border-clamped (0x%08X)",
          g_smp0.border_color);
    unit0(0x100000, 0x85, 1, WRAP(3, 4, 1), 0xAAE4, lin, 0x80FF0000u, (4u << 16) | 1u);
    draw_triangle();
    CHECK(g_smp0.border_color == 0xFF000000u, "1D ignores T border (0x%08X)",
          g_smp0.border_color);
    /* A=ONE, R=ZERO, G<-R, B<-A */
    unit0(0x100000, 0x85, 2, WRAP(4, 4, 1), 0xA110, lin, 0x40C08020u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0xFF00C040u, "border goes through the crossbar (0x%08X)",
          g_smp0.border_color);
    /* R signed: (v - 128) & 0xFF on the R byte */
    unit0(0x100000, 0x85, 2, WRAP(4, 4, 1), 0xAAE4, lin | (2u << 28), 0xFF102030u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0xFF902030u, "sign-extended lane is re-biased (0x%08X)",
          g_smp0.border_color);

    printf("-- custom border colour floats (remapped_border_color, require_border_color_remap)\n");
#define NEAR(a, b) ((a) - (b) < 1e-6f && (b) - (a) < 1e-6f)
    unit0(0x100000, 0x85, 2, WRAP(4, 4, 1), 0xA110, lin, 0x40C08020u, RECT4);
    draw_triangle();
    CHECK(NEAR(g_smp0.border_rgba[0], 0.0f) && NEAR(g_smp0.border_rgba[1], 192.0f / 255.0f) &&
          NEAR(g_smp0.border_rgba[2], 64.0f / 255.0f) && NEAR(g_smp0.border_rgba[3], 1.0f),
          "border_rgba is the crossbarred colour, R,G,B,A (%g %g %g %g)", g_smp0.border_rgba[0],
          g_smp0.border_rgba[1], g_smp0.border_rgba[2], g_smp0.border_rgba[3]);
    rsx_draw_engine_set_border_color_remap(0);
    unit0(0x100000, 0x85, 2, WRAP(4, 4, 1), 0xA110, lin, 0x40C08021u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0x40C08021u && NEAR(g_smp0.border_rgba[0], 192.0f / 255.0f) &&
          NEAR(g_smp0.border_rgba[3], 64.0f / 255.0f),
          "a device swizzling the border itself gets the raw colour (0x%08X)", g_smp0.border_color);
    rsx_draw_engine_set_border_color_remap(1);
    unit0(0x100000, 0x85, 2, WRAP(1, 1, 1), 0xAAE4, lin, 0x40C08020u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0xFF000000u && NEAR(g_smp0.border_rgba[3], 1.0f) &&
          NEAR(g_smp0.border_rgba[0], 0.0f), "not border-clamped: opaque black floats");
    /* X16: decoded_remap rewrites the crossbar, but remapped_border_color
     * tests the raw register, so an identity CONTROL1 leaves the colour. */
    unit0(0x100000, 0x92, 2, WRAP(4, 4, 1), 0xAAE4, lin, 0x40C08020u, RECT4);
    draw_triangle();
    CHECK(g_smp0.border_color == 0x40C08020u,
          "identity CONTROL1 skips the crossbar whatever the format (0x%08X)", g_smp0.border_color);

    printf("-- colour write mask by surface format (decode_blend_state, get_write_output_mask)\n");
    {
        static const struct { u32 fmt, mask, mrt, want, want_mrt; } cm[] = {
            { 0x08u, 0x01010101u, 0xFFF0u, 0x01010101u, 0xFFF0u },   /* A8R8G8B8: all */
            { 0x05u, 0x01010101u, 0xFFF0u, 0x01010101u, 0xFFF0u },   /* X8R8G8B8: all */
            { 0x0Cu, 0x01010101u, 0xFFF0u, 0x01010101u, 0xFFF0u },   /* W32: all */
            { 0x03u, 0x01010101u, 0xFFF0u, 0x00010101u, 0xEEE0u },   /* R5G6B5: no A */
            { 0x09u, 0x01010101u, 0xFFF0u, 0x00000001u, 0x8880u },   /* B8: .b only */
            { 0x0Au, 0x01010101u, 0xFFF0u, 0x00000101u, 0xCCC0u },   /* G8B8: .g .b */
            { 0x0Du, 0x01010101u, 0xFFF0u, 0x00010000u, 0x2220u },   /* X32: .r */
            { 0x09u, 0x01010100u, 0x7770u, 0x00000000u, 0x0000u },   /* B8 without B */
        };
        for (u32 i = 0; i < sizeof cm / sizeof cm[0]; i++) {
            u32 mk = cm[i].mask, mrt = cm[i].mrt;
            rsx_draw_engine_trim_color_mask(cm[i].fmt, &mk, &mrt);
            CHECK(mk == cm[i].want && mrt == cm[i].want_mrt,
                  "format 0x%02X: mask 0x%08X mrt 0x%04X", cm[i].fmt, mk, mrt);
        }
        m(M_SURFACE_FORMAT, 0x09u);
        m(M_COLOR_MASK, 0x01010101u);
        draw_triangle();
        CHECK(stub.pipeline_rs.color_mask == 0x00000001u,
              "a B8 target's pipeline writes .b only (0x%08X)", stub.pipeline_rs.color_mask);
        m(M_SURFACE_FORMAT, 0x08u);
        m(M_COLOR_MASK, 0x01000101u);
        draw_triangle();
        CHECK(stub.pipeline_rs.color_mask == 0x01000101u,
              "an A8R8G8B8 target's pipeline keeps the register (0x%08X)", stub.pipeline_rs.color_mask);
        m(M_COLOR_MASK, 0x01010101u);
    }

    printf("-- 1D textures are one texel high (fragment_texture::height)\n");
    unit0(0x110000, 0x85, 1, WRAP(1, 1, 1), 0xAAE4, lin, 0, (8u << 16) | 8u);
    draw_triangle();
    CHECK(g_created_w == 8 && g_created_h == 1, "1D created %ux%u", g_created_w, g_created_h);

    printf("-- compressed formats never read a render target (fast_texture_search)\n");
    m(M_CLEAR_BUFFERS, 0xF0u);
    draw_triangle();
    const u32 rt = stub.bound_surface;
    m(M_COLOR_A_OFFSET, 0x80000u);
    /* Linear at the target's pitch: check_framebuffer_resource wants a pitch
     * compatible texture (scratch/parity/texcache.md). */
    unit0(0x40000, 0x85 | 0x20, 2, WRAP(1, 1, 1), 0xAAE4, lin, 0, RECT4);
    m(0x1840 /* control3: pitch */, 1024u);
    draw_triangle();
    CHECK(rt && stub.bound_tex[0] == rt, "A8R8G8B8 over the target samples it");
    const int created = stub.n_texture_create;
    unit0(0x40000, 0x86, 2, WRAP(1, 1, 1), 0x01FF, lin, 0, RECT4);
    draw_triangle();
    CHECK(stub.bound_tex[0] != rt && stub.n_texture_create == created + 1,
          "DXT1 over the target is decoded from memory");

    engine_down();
    free(vm_base);
    printf("texture parity failures: %d\n", g_failures);
    return g_failures ? 1 : 0;
}
