// launcher_extra.inc -- PC extras for the launcher (included by launcher.cpp):
//   * the options behind the game's "More PC Options" menu (r2_pcextra.cpp), so the in-game menu and the
//     launcher write the same settings.ini keys / settings.bat values,
//   * controller button layout (PAD_MAP_*, PAD_SOUTHPAW; libs/input/cellPad.c XInput hook),
//   * the "Saves & tools" page: save list, back up / restore / open, automatic backup before Play
//     (keeps the newest N), and the built-in benchmark (R2_BENCH=1).
// Needs from launcher.cpp: add_option, g_set, path_of, toast, add, deco, SF, layout_section, play, g_hw, g_dir.
#ifndef LX_ENV
#define LX_ENV(x) "R2_" x
#define LX_TAG "R2"
#endif
#ifndef LX_BG_ENV
#define LX_BG_ENV "R2_BACKGROUND"       // the launcher's "When the game is in the background" variable
#endif
#ifndef LX_HAS_FX
#define LX_HAS_FX 1                     // the game's post-effect toggles (r2_pcextra.cpp s_fx)
#endif

// Frame Generation: keep only the multipliers the detected GPU can run (RTX 40: 2x; RTX 50: up to 4x; other GPUs:
// FSR 3 2x). The game checks again at start (Streamline / FidelityFX) and logs why it is off.
static void lx_framegen_filter()
{
    for (auto& s : g_set) {
        if (strcmp(s.key, "dlss_fg") != 0) continue;
        int series = 0;
        const char* g = g_hwi.gpu;
        if (g_hwi.vendor == 0x10DE) {
            for (const char* p = strstr(g, "RTX "); p; p = strstr(p + 4, "RTX ")) {
                const char* d = p + 4;
                if ((d[0] == '4' || d[0] == '5') && d[1] >= '0' && d[1] <= '9' && d[2] >= '0' && d[2] <= '9' && d[3] >= '0' && d[3] <= '9')
                    series = d[0] - '0';
            }
            if (strstr(g, "Blackwell")) series = 5;
            else if (strstr(g, "Ada") && !series) series = 4;
        }
        const int max_mult = series == 5 ? 4 : series == 4 ? 2 : 1;
        std::vector<Choice> keep;
        for (const auto& c : s.choices) {
            const bool fsr = strcmp(c.value, "fsr3") == 0;
            if (!*c.value || (fsr ? series < 4 : atoi(c.value) <= max_mult)) keep.push_back(c);
        }
        s.choices = keep;
        if (s.def >= (int)s.choices.size()) s.def = 0;
        if (s.sel >= (int)s.choices.size()) s.sel = 0;
    }
}

static void register_extra_options()
{
    // Display / Upscaling / Interface / Advanced pages (launcher_cats.inc)
    add_option(SEC_X_DISPLAY, "resizable", "Resizable window",
               "Windowed mode: drag the window edges to resize it; the picture follows the new size.",
               "RSX_RESIZABLE", { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "altenter", "Alt+Enter",
               "Alt+Enter switches between windowed and borderless fullscreen in game.",
               "RSX_ALT_ENTER", { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "hdr", "HDR output",
               "Experimental. Needs Windows HDR on (Settings > Display > Use HDR). Draws the final image into an HDR10 "
               "(PQ) or scRGB swapchain: the game's SDR image stays at the paper-white brightness below and only "
               "highlights are expanded towards the peak. Auto turns HDR on only when Windows HDR is on for the game's "
               "display. Falls back to SDR otherwise. The performance overlay and screenshots stay SDR. Pros: brighter "
               "highlights and deeper contrast. Cons: needs a real HDR display; wrong brightness settings look washed "
               "out or clip highlights.",
               LX_ENV("HDR"), { {"Off", ""}, {"Auto", "auto"}, {"On  (HDR10)", "1"}, {"On  (scRGB)", "scrgb"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "hdr_paper", "HDR paper white",
               "How bright the HUD, menus and the game's normal image are in HDR, in nits (203 is the HDR reference "
               "white). Lower for a dark room, higher for a bright one.",
               LX_ENV("HDR_PAPER"), { {"203 nits", ""}, {"100 nits", "100"}, {"160 nits", "160"}, {"250 nits", "250"},
                                      {"300 nits", "300"}, {"400 nits", "400"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "hdr_peak", "HDR peak brightness",
               "How bright the brightest highlights get, in nits (also sent to the display as HDR metadata). Display = "
               "the peak Windows reports for the monitor. Set it equal to paper white to turn highlight expansion off.",
               LX_ENV("HDR_PEAK"), { {"Display", ""}, {"400 nits", "400"}, {"600 nits", "600"}, {"800 nits", "800"},
                                     {"1000 nits", "1000"}, {"1500 nits", "1500"}, {"2000 nits", "2000"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "hdr_sat", "HDR saturation",
               "Colour saturation of the HDR picture (1.0 = as the game).",
               LX_ENV("HDR_SAT"), { {"1.0", ""}, {"0.9", "0.9"}, {"1.1", "1.1"}, {"1.2", "1.2"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "reflex", "NVIDIA Reflex",
               "Experimental. Lower input lag on GeForce cards (Vulkan VK_NV_low_latency2, driver 535 or newer): the "
               "game waits for the GPU before it starts a frame. It is not a frame limiter - the Frame rate setting "
               "still decides the cap. Does nothing on other GPUs.",
               LX_ENV("REFLEX"), { {"Off", ""}, {"On", "1"}, {"On + Boost", "2"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "dlss_fg", "Frame Generation",
               "NVIDIA DLSS Frame Generation: the GPU draws extra frames between the game's own, so motion looks "
               "smoother when the CPU is the limit (big fights). 2x shows two frames per game frame, 3x / 4x "
               "(Multi Frame Generation) three or four - 4x is for 165 Hz+ displays. Adds a little input lag "
               "(Reflex is turned on with it) and needs a GeForce RTX 40 or 50 card, Anti-aliasing set to NVIDIA "
               "DLSS and SDR or HDR10 output. The HUD is kept out of the generated frames. Turns itself off in menus, "
               "movies, loading screens and split-screen. Only the multipliers your GPU supports are listed. "
               "FSR 3 2x (other GPUs): AMD FidelityFX frame interpolation, two frames per game frame; needs Anti-"
               "aliasing set to FSR, DLSS or XeSS and SDR or HDR10 output; no Reflex, so it adds a bit more lag. "
               LX_ENV("FG_BACKEND") "=auto|dlss|fsr3 picks the backend by hand.",
               LX_ENV("DLSS_FG"), { {"Off", ""}, {"2x", "2"}, {"3x", "3"}, {"4x", "4"}, {"FSR 3 2x", "fsr3"} }, 0, {}, "rpcs3");
    add_option(SEC_X_DISPLAY, "discord", "Discord Rich Presence",
               "Shows \"Playing Resistance\" and the level in your Discord status. Needs the Discord app running and "
               "an application id in discord_appid.txt (or the " LX_ENV("DISCORD_APPID") " variable); does nothing otherwise.",
               LX_ENV("DISCORD"), { {"Off", ""}, {"On", "1"} }, 0);
    add_option(SEC_X_DISPLAY, "photo", "Photo mode  (F7)",
               "F7 in game pauses, hides the port's overlays and lets you take clean F12 screenshots. "
               "No free camera (see the README).",
               LX_ENV("PHOTO"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "cutscene_skip", "Hold to skip movies",
               "Hold Space / Enter / A / Menu for a second to skip a pre-rendered movie (the game's own skip).",
               LX_ENV("CUTSCENE_SKIP"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_DISPLAY, "savebackup", "Back up saves before playing",
               "Play copies your saved games to save_backups\\ first (only when they changed), keeping the newest "
               "ones. See the Saves & tools page.",
               LX_ENV("SAVE_BACKUP"), { {"On  (keep 10)", ""}, {"On  (keep 5)", "5"}, {"On  (keep 30)", "30"}, {"Off", "0"} }, 0);

    // Graphics page "EFFECTS AND ACCESSIBILITY", the crosshair on Controls (all also in game: F10 > More PC Options, or F9)
#if LX_HAS_FX
    add_option(SEC_X_FX, "fx_blur", "Screen blur",
               "The game's full-screen blur effects (damage, sprint, explosions). Off keeps the picture sharp.",
               LX_ENV("FX_SCREEN_BLUR"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_shake", "Camera shake",
               "Explosion and impact camera shake.", LX_ENV("FX_CAMERA_SHAKE"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_letterbox", "Cinematic black bars",
               "Black bars at the top and bottom during in-engine cutscenes.",
               LX_ENV("FX_LETTERBOX"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
#endif
#if defined(LX_HAS_PFX) && LX_HAS_PFX
    add_option(SEC_X_FX, "fx_filmgrain", "Film grain",
               "The game's film grain noise. Off gives a clean picture.",
               LX_ENV("FX_FILMGRAIN"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_dof", "Depth of field",
               "Blur of the background / foreground in cutscenes, while aiming and in some areas.",
               LX_ENV("FX_DOF"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_FX, "fx_motionblur", "Motion blur",
               "Camera motion blur and the blur on fast-moving objects.",
               LX_ENV("FX_MOTIONBLUR"), { {"On  (PS3 original)", ""}, {"Off", "0"} }, 0);
#endif
    add_option(SEC_X_FX, "colorblind", "Colour-blind filter",
               "Shifts colours so they are easier to tell apart (daltonization of the final image).",
               LX_ENV("COLORBLIND"), { {"Off", ""}, {"Protanopia  (red)", "1"}, {"Deuteranopia  (green)", "2"},
                                       {"Tritanopia  (blue)", "3"} }, 0, {}, "rpcs3");
    add_option(SEC_X_FX, "crosshair", "Extra crosshair",
               "A small dot or cross at the screen centre while on foot, for when the game's reticle is hard to see.",
               LX_ENV("CROSSHAIR"), { {"Off", ""}, {"Dot", "dot"}, {"Cross", "cross"} }, 0);

    // Controls page: "CONTROLLER" and "CONTROLLER BUTTONS"
    add_option(SEC_X_PAD, "pad_disconnect_pause", "Pause when the controller disconnects",
               "Pauses the game and shows a message when the controller you play with is unplugged or runs out of "
               "battery. Reconnect it, or press Enter, to continue.",
               LX_ENV("PAD_DISCONNECT_PAUSE"), { {"On", ""}, {"Off", "0"} }, 0);
    add_option(SEC_X_PAD, "pad_southpaw", "Swap sticks  (southpaw)",
               "Left stick looks, right stick moves.", "PAD_SOUTHPAW", { {"Off", ""}, {"On", "1"} }, 0);
    static const char* keys[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START" };
    static const char* names[] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Left stick click", "Right stick click",
                                   "View", "Menu" };
    for (int i = 0; i < 12; i++) {
        std::vector<Choice> ch = { { "Default", "" } };
        for (int j = 0; j < 12; j++) {
            if (j == i) continue;
            char* l = (char*)malloc(48); snprintf(l, 48, "Acts as %s", names[j]);
            ch.push_back({ l, keys[j] });
        }
        char* key = (char*)malloc(24); snprintf(key, 24, "padmap_%s", keys[i]);
        for (char* c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
        char* env = (char*)malloc(24); snprintf(env, 24, "PAD_MAP_%s", keys[i]);
        char* title = (char*)malloc(48); snprintf(title, 48, "Button %s", names[i]);
        add_option(SEC_X_PAD, key, title,
                   "What this controller button does in game (Xbox names; a PlayStation pad through Steam Input / "
                   "DS4Windows maps the same way). Swapping two buttons needs both rows. Also in game: F10 > More PC "
                   "Options > Controller layout.", env, std::move(ch), 0);
    }
}

static const char* lx_val(const char* key)
{
    for (auto& s : g_set) if (!strcmp(s.key, key)) return s.choices[s.sel].value;
    return "";
}

// ---- saves -----------------------------------------------------------------------------------------
static std::string lx_save_dir()
{
    const char* p = getenv("PS3_GAMEDATA_ROOT");
    return ((p && *p) ? std::string(p) : path_of("gamedata")) + "\\dev_hdd0\\home\\00000001\\savedata";
}
static std::string lx_backup_root() { return path_of("save_backups"); }
struct LxEntry { std::string name, title; ULONGLONG newest = 0, bytes = 0; int files = 0; };
static void lx_scan(const std::string& dir, LxEntry& e)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { lx_scan(dir + "\\" + fd.cFileName, e); continue; }
        const ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        e.newest = std::max(e.newest, t);
        e.bytes += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.files++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
// PARAM.SFO: SUB_TITLE (or TITLE) of a PS3 save
static std::string lx_sfo_title(const std::string& dir)
{
    FILE* f = fopen((dir + "\\PARAM.SFO").c_str(), "rb");
    if (!f) return "";
    std::vector<unsigned char> b(65536);
    const size_t n = fread(b.data(), 1, b.size(), f); fclose(f);
    if (n < 20 || memcmp(b.data(), "\0PSF", 4)) return "";
    auto u32 = [&](size_t o) { return o + 4 <= n ? (unsigned)b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (unsigned)b[o + 3] << 24 : 0u; };
    auto u16 = [&](size_t o) { return o + 2 <= n ? (unsigned)(b[o] | b[o + 1] << 8) : 0u; };
    const unsigned kt = u32(8), dt = u32(12), cnt = u32(16);
    std::string sub, title;
    for (unsigned i = 0; i < cnt && i < 64; i++) {
        const size_t ent = 20 + i * 16;
        const size_t ko = kt + u16(ent), dof = dt + u32(ent + 12), len = u32(ent + 4);
        if (ko >= n || dof + len > n) continue;
        const char* k = (const char*)&b[ko];
        std::string v((const char*)&b[dof], strnlen((const char*)&b[dof], len));
        if (!strcmp(k, "SUB_TITLE")) sub = v; else if (!strcmp(k, "TITLE")) title = v;
    }
    return !sub.empty() ? sub : title;
}
static std::vector<LxEntry> lx_list(const std::string& root, bool saves)
{
    std::vector<LxEntry> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        LxEntry e; e.name = fd.cFileName;
        lx_scan(root + "\\" + e.name, e);
        if (saves) e.title = lx_sfo_title(root + "\\" + e.name);
        out.push_back(e);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [&](const LxEntry& a, const LxEntry& b) { return saves ? a.name < b.name : a.name > b.name; });
    return out;
}
static bool lx_copy_tree(const std::string& from, const std::string& to)
{
    CreateDirectoryA(to.c_str(), NULL);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((from + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        const std::string a = from + "\\" + fd.cFileName, b = to + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = lx_copy_tree(a, b) && ok;
        else ok = CopyFileA(a.c_str(), b.c_str(), FALSE) && ok;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
static void lx_delete_tree(const std::string& dir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            const std::string p = dir + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) lx_delete_tree(p);
            else { SetFileAttributesA(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(p.c_str()); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir.c_str());
}
// a backup is save_backups\<YYYY-MM-DD_HHMMSS>_<kind>\<the savedata folders>; kind = auto / manual / before-restore
static std::string lx_backup(const char* kind)
{
    LxEntry cur; lx_scan(lx_save_dir(), cur);
    if (!cur.files) return "";
    CreateDirectoryA(lx_backup_root().c_str(), NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char name[96];
    snprintf(name, sizeof name, "%04u-%02u-%02u_%02u%02u%02u_%s", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind);
    const std::string dst = lx_backup_root() + "\\" + name;
    if (!lx_copy_tree(lx_save_dir(), dst)) { lx_delete_tree(dst); return ""; }
    return name;
}
// before Play: back up when the saves changed since the newest backup; keep the newest N automatic ones
static void lx_auto_backup()
{
    const char* v = lx_val("savebackup");
    if (!strcmp(v, "0")) return;
    const int keep = *v ? atoi(v) : 10;
    LxEntry cur; lx_scan(lx_save_dir(), cur);
    if (!cur.files) return;
    auto bk = lx_list(lx_backup_root(), false);
    for (auto& b : bk) if (b.newest >= cur.newest && b.files == cur.files && b.bytes == cur.bytes) return;   // unchanged
    lx_backup("auto");
    bk = lx_list(lx_backup_root(), false);      // newest first
    int n = 0;
    for (auto& b : bk) {
        if (b.name.size() < 5 || b.name.compare(b.name.size() - 5, 5, "_auto") != 0) continue;
        if (++n > keep) lx_delete_tree(lx_backup_root() + "\\" + b.name);
    }
}
static std::vector<LxEntry> g_lx_saves, g_lx_backups;
static DWORD g_lx_loaded = 0;
static void lx_refresh(bool force)
{
    if (!force && g_lx_loaded && GetTickCount() - g_lx_loaded < 3000) return;
    g_lx_loaded = GetTickCount();
    g_lx_saves = lx_list(lx_save_dir(), true);
    g_lx_backups = lx_list(lx_backup_root(), false);
}
static std::string lx_when(ULONGLONG ft)
{
    if (!ft) return "";
    FILETIME f = { (DWORD)ft, (DWORD)(ft >> 32) }, l; SYSTEMTIME s;
    FileTimeToLocalFileTime(&f, &l); FileTimeToSystemTime(&l, &s);
    char b[64]; snprintf(b, sizeof b, "%04u-%02u-%02u %02u:%02u", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute);
    return b;
}
static std::string lx_size(ULONGLONG b) { char s[32]; snprintf(s, sizeof s, b > 1048576 ? "%.1f MB" : "%.0f KB", b > 1048576 ? b / 1048576.0 : b / 1024.0); return s; }

// ---- benchmark ---------------------------------------------------------------------------------------
static std::string lx_last_bench()
{
    WIN32_FIND_DATAA fd; std::string best; FILETIME bt = {};
    HANDLE h = FindFirstFileA(path_of("logs\\bench\\" LX_TAG "_bench_*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "";
    do { if (CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) { bt = fd.ftLastWriteTime; best = fd.cFileName; } } while (FindNextFileA(h, &fd));
    FindClose(h);
    FILE* f = fopen(path_of(("logs\\bench\\" + best).c_str()).c_str(), "r");
    if (!f) return "";
    std::string avg, low, usd, line; char b[256];
    while (fgets(b, sizeof b, f)) {
        line = b; line.erase(line.find_last_not_of("\r\n ") + 1);
        auto v = [&](const char* k) { return line.compare(0, strlen(k), k) == 0 ? line.substr(line.find_first_not_of(' ', strlen(k))) : std::string(); };
        if (!v("avg fps").empty()) avg = v("avg fps");
        if (!v("1% low fps").empty()) low = v("1% low fps");
        if (!v("us/draw").empty()) usd = v("us/draw");
    }
    fclose(f);
    return "Last run (" + lx_when(((ULONGLONG)bt.dwHighDateTime << 32) | bt.dwLowDateTime) + "):  " + avg + " fps average,  " +
           low + " fps 1% low,  " + usd + " \xC2\xB5s per draw";
}

enum { BT_X_BACKUP = 100, BT_X_OPEN_BACKUPS, BT_X_BENCH, BT_X_BENCH_LOGS, BT_X_RESTORE, BT_X_DELETE };

static void extra_layout_tools(float x, float& y, float w)
{
    lx_refresh(false);
    deco(D_H1, RectF(x, y, w, SF(34)), "Saves & tools"); y += SF(36);
    deco(D_INTRO, RectF(x, y, w, SF(20)), "Back up and restore saved games, and measure performance.");
    y += SF(40);
    deco(D_H2, RectF(x, y, w, SF(22)), "SAVED GAMES"); y += SF(30);
    if (g_lx_saves.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No saved games yet."); y += SF(30); }
    for (size_t i = 0; i < g_lx_saves.size(); i++) {
        const LxEntry& e = g_lx_saves[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(34)), -300 - (int)i);
        row.title = e.name; row.help = e.title.empty() ? e.name : e.title + "  (" + e.name + ")";
        deco(D_LABEL, RectF(x, y, w - SF(260), SF(34)), e.title.empty() ? e.name : e.title);
        deco(D_LABEL_DIM, RectF(x + w - SF(250), y, SF(250), SF(34)), lx_when(e.newest) + "  \xC2\xB7  " + lx_size(e.bytes));
        y += SF(36);
    }
    y += SF(8);
    Widget& b1 = add(W_BUTTON, RectF(x, y, SF(150), SF(38)), BT_X_BACKUP);
    b1.title = "Back up now"; b1.help = "Copies all saved games to save_backups\\ (kept until you delete them).";
    Widget& b2 = add(W_BUTTON, RectF(x + SF(162), y, SF(170), SF(38)), BT_SAVES);
    b2.title = "Open save folder"; b2.help = "The game's save folder (gamedata\\dev_hdd0\\home\\00000001\\savedata).";
    Widget& b3 = add(W_BUTTON, RectF(x + SF(344), y, SF(190), SF(38)), BT_X_OPEN_BACKUPS);
    b3.title = "Open backups folder"; b3.help = "save_backups\\ in the game folder.";
    y += SF(56);
    deco(D_H2, RectF(x, y, w, SF(22)), "BACKUPS"); y += SF(30);
    if (g_lx_backups.empty()) { deco(D_NOTE, RectF(x, y, w, SF(22)), "No backups yet. Play makes one automatically (Advanced page, "
                                                                     "\"Back up saves before playing\")."); y += SF(30); }
    for (size_t i = 0; i < g_lx_backups.size() && i < 40; i++) {
        const LxEntry& e = g_lx_backups[i];
        Widget& row = add(W_ROW, RectF(x - SF(12), y, w + SF(24), SF(40)), -400 - (int)i);
        row.title = e.name; row.help = "Backup " + e.name + ": " + std::to_string(e.files) + " files, " + lx_size(e.bytes) + ".";
        deco(D_LABEL, RectF(x, y, w - SF(230), SF(40)), e.name);
        Widget& r = add(W_BUTTON, RectF(x + w - SF(220), y + SF(4), SF(110), SF(32)), BT_X_RESTORE, (int)i);
        r.title = "Restore"; r.help = "Replaces your current saved games with this backup (they are backed up first).";
        Widget& d = add(W_BUTTON, RectF(x + w - SF(100), y + SF(4), SF(100), SF(32)), BT_X_DELETE, (int)i);
        d.title = "Delete"; d.help = "Deletes this backup.";
        y += SF(42);
    }
    y += SF(24);
    deco(D_H2, RectF(x, y, w, SF(22)), "BENCHMARK"); y += SF(30);
    deco(D_NOTE, RectF(x, y, w, SF(60)), "Starts the game with your settings, goes from the title screen into the first level by "
                                         "itself, measures for 60 seconds, writes logs\\bench\\ and closes the game. Don't touch "
                                         "the keyboard or controller while it runs (about 4 minutes).");
    y += SF(66);
    Widget& b4 = add(W_BUTTON, RectF(x, y, SF(170), SF(38)), BT_X_BENCH);
    b4.title = "Run benchmark"; b4.help = "Average fps, 1% low fps and GPU time per draw call, to logs\\bench\\.";
    Widget& b5 = add(W_BUTTON, RectF(x + SF(182), y, SF(170), SF(38)), BT_X_BENCH_LOGS);
    b5.title = "Open results"; b5.help = "logs\\bench\\: one report per run and bench_history.csv.";
    y += SF(46);
    const std::string last = lx_last_bench();
    if (!last.empty()) { deco(D_LABEL_DIM, RectF(x, y, w, SF(28)), last); y += SF(30); }
}

// returns true when the button was ours
static bool extra_click(int id, int sub)
{
    if (id == BT_X_BACKUP) {
        const std::string n = lx_backup("manual");
        toast(n.empty() ? "Nothing to back up (no saved games yet)." : "Saved games backed up to save_backups\\" + n);
        lx_refresh(true);
    } else if (id == BT_X_OPEN_BACKUPS) {
        CreateDirectoryA(lx_backup_root().c_str(), NULL);
        ShellExecuteA(g_hw, "open", lx_backup_root().c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else if (id == BT_X_RESTORE || id == BT_X_DELETE) {
        lx_refresh(true);
        if (sub < 0 || sub >= (int)g_lx_backups.size()) return true;
        const std::string name = g_lx_backups[sub].name, src = lx_backup_root() + "\\" + name;
        if (id == BT_X_DELETE) {
            if (MessageBoxA(g_hw, ("Delete the backup " + name + "?").c_str(), "Saves", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                lx_delete_tree(src); toast("Backup deleted.");
            }
        } else if (MessageBoxA(g_hw, ("Replace your saved games with the backup " + name + "?\n\nYour current saves are backed "
                                      "up first (\"before-restore\"). Don't do this while the game is running.").c_str(),
                               "Restore saves", MB_YESNO | MB_ICONWARNING) == IDYES) {
            lx_backup("before-restore");
            const std::string sd = lx_save_dir();
            for (auto& s : lx_list(sd, true)) lx_delete_tree(sd + "\\" + s.name);
            std::string p = (getenv("PS3_GAMEDATA_ROOT") && *getenv("PS3_GAMEDATA_ROOT")) ? getenv("PS3_GAMEDATA_ROOT") : path_of("gamedata");
            for (const char* part : { "\\dev_hdd0", "\\home", "\\00000001", "\\savedata" }) { p += part; CreateDirectoryA(p.c_str(), NULL); }
            toast(lx_copy_tree(src, sd) ? "Saved games restored from " + name + "." : "Restore failed - see save_backups\\.");
        }
        lx_refresh(true);
    } else if (id == BT_X_BENCH) {
        if (MessageBoxA(g_hw, "Run the benchmark now?\n\nThe game starts, goes into the first level by itself, measures for 60 "
                              "seconds and closes. Don't press anything while it runs.", "Benchmark",
                        MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return true;
        SetEnvironmentVariableA(LX_ENV("BENCH"), "1");
        SetEnvironmentVariableA(LX_BG_ENV, "run");   // keeps going if the window loses focus
        SetEnvironmentVariableA(LX_ENV("PAD_DISCONNECT_PAUSE"), "0");
        play();
    } else if (id == BT_X_BENCH_LOGS) {
        CreateDirectoryA(path_of("logs").c_str(), NULL); CreateDirectoryA(path_of("logs\\bench").c_str(), NULL);
        ShellExecuteA(g_hw, "open", path_of("logs\\bench").c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else return false;
    return true;
}
