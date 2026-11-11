// Launcher categories: which options each settings page lists, and under which heading.
//
// Options are looked up by their settings.ini key, so the same table serves Resistance 2 and Resistance 3:
// a key the port does not have is skipped, and a group with none of its keys is left out. "prefix*" matches
// every key that starts with prefix. The section an option was registered with (add_option) is unchanged
// and still decides what Auto-detect resets; only where it is shown comes from here. Any option no group
// lists appears on the Advanced page under OTHER, so nothing can go missing.
struct CatGroup { int page; const char* head; const char* keys; };
static const CatGroup g_cat[] = {
    { P_DISPLAY,  "SCREEN",                        "display window monitor resizable altenter aspect hudfix" },
    { P_DISPLAY,  "FRAME RATE",                    "fps vsync menufps present refreshswitch" },
    { P_DISPLAY,  "HDR",                           "hdr hdr_paper hdr_peak hdr_sat" },
    { P_GRAPHICS, "RESOLUTION",                    "res filter" },
    { P_GRAPHICS, "QUALITY",                       "aniso shadowscale fxfullres ssao fullres lodscale noloadshed" },
    { P_GRAPHICS, "VIEW",                          "fov" },
    { P_GRAPHICS, "EFFECTS AND ACCESSIBILITY",     "fx_* colorblind" },
    { P_GRAPHICS, "RENDERER",                      "renderer" },
    { P_UPSCALE,  "ANTI-ALIASING AND UPSCALING",   "fsr upscale sharpen objmv" },
    { P_UPSCALE,  "FRAME GENERATION AND LATENCY",  "dlss_fg reflex" },
    { P_PERF,     "PROCESSOR",                     "cpu" },
    { P_PERF,     "SHADER CACHE",                  "shaders precompile shaderthreads shaderinterp" },
    { P_CONTROLS, "AIMING AND STICKS",             "msensy adssens aimtoggle deadzone curve crosshair" },
    { P_CONTROLS, "CONTROLLER",                    "padbackend padglyphs nswap p1pad lightbar pad_disconnect_pause pad_southpaw" },
    { P_CONTROLS, "GYRO",                          "gyro gyrosens gyroinvy gyrospace gyrocal" },
    { P_CONTROLS, "LOCAL CO-OP",                   "coop p2pad p2kb" },
    { P_CONTROLS, "CONTROLLER BUTTONS",            "padmap_*" },
    { P_AUDIO,    "SOUND",                         "speakers audio_buffer" },
    { P_AUDIO,    "IN THE BACKGROUND",             "background" },
    { P_UI,       "NOTIFICATIONS",                 "trophytoast" },
    { P_UI,       "MOVIES",                        "cutscene_skip skiphint" },
    { P_UI,       "OVERLAYS",                      "overlay" },
    { P_UI,       "PHOTO MODE AND DISCORD",        "photo discord" },
    { P_ADVANCED, "START-UP",                      "skipintro" },
    { P_ADVANCED, "MODS AND TEXTURES",             "mods texpack texdump" },
    { P_ADVANCED, "SAVES",                         "savebackup" },
    { P_ADVANCED, "UPDATES",                       "updates" },
};
static const char* g_page_title[P_COUNT] = { "Display", "Graphics", "Upscaling & Frame Generation", "Performance",
                                             "Controls", "Audio", "Interface & Notifications", "Advanced",
                                             "Trophies", "Saves & tools", "About" };
static const char* g_page_intro[P_COUNT] = {
    "How the game window looks and how fast it runs.",
    "Image quality. Higher settings look better and cost more GPU time.",
    "Anti-aliasing, DLSS / FSR / XeSS upscaling, sharpening and frame generation.",
    "How the game uses your processor and builds its shaders.",
    "",
    "Sound output, and what the game does while it is in the background.",
    "Notifications, hints and overlays the game shows while you play.",
    "Start-up, mods, saves and updates.",
    "", "", "" };

// calls f(index into g_set) for every option the space-separated key list names, in list order
template <class F> static void cat_each(const char* keys, F f)
{
    for (const char* p = keys; *p;) {
        while (*p == ' ') p++;
        const char* e = p; while (*e && *e != ' ') e++;
        if (e > p) {
            const bool pre = e[-1] == '*';
            const size_t n = (size_t)(e - p) - (pre ? 1 : 0);
            for (size_t i = 0; i < g_set.size(); i++) {
                const char* k = g_set[i].key;
                if (pre ? strncmp(k, p, n) == 0 : (strlen(k) == n && strncmp(k, p, n) == 0)) {
                    f((int)i);
                    if (!pre) break;
                }
            }
        }
        p = e;
    }
}
static bool cat_placed(int si)
{
    bool found = false;
    for (auto& g : g_cat) cat_each(g.keys, [&](int i) { if (i == si) found = true; });
    return found;
}
static void cat_group(float x, float& y, float w, const char* head, const std::vector<int>& rows, bool& first)
{
    if (rows.empty()) return;
    if (!first) y += SF(18);
    first = false;
    deco(D_H2, RectF(x, y, w, SF(22)), head); y += SF(26);
    for (int si : rows) layout_setting_row(x, y, w, si);
}
// the groups of one page; the Advanced page also lists every option no group places (OTHER)
static void layout_cat_groups(float x, float& y, float w, int page, bool first = true)
{
    for (auto& g : g_cat) {
        if (g.page != page) continue;
        std::vector<int> rows;
        cat_each(g.keys, [&](int i) { rows.push_back(i); });
        cat_group(x, y, w, g.head, rows, first);
    }
    if (page == P_ADVANCED) {
        std::vector<int> rows;
        for (int i = 0; i < (int)g_set.size(); i++) if (!cat_placed(i)) rows.push_back(i);
        cat_group(x, y, w, "OTHER", rows, first);
    }
}
