// ===========================================================================
// launcher_dist.inc -- distribution extras for Launcher.exe (included by launcher.cpp)
//
//  * Update check: asks GitHub for the newest release of DIST_REPO (one unauthenticated request when the
//    launcher opens) and offers to open the download page when it is newer than the VERSION file next to
//    Launcher.exe. Option "Check for updates" (On by default). Nothing is downloaded or replaced.
//  * Wine / Proton and Steam Deck: adds a 40 fps limit (for the Deck's 40 Hz mode) and a 1280 x 800 window
//    size; on the Deck the controller's A / Start button presses Play.
//  * Settings backup (DIST_USER_DIR): see dist_config_restore / dist_config_save below.
//
// launcher.cpp calls dist_init() before the settings are loaded and dist_start(hwnd) once the window is up.
// ===========================================================================
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#ifndef DIST_REPO
#error "define DIST_REPO (owner/name) before including launcher_dist.inc"
#endif

static bool g_dist_wine = false, g_dist_deck = false;
static HWND g_dist_hw = NULL;
static volatile LONG g_dist_upd_done = 0;       // 1: the check finished (g_dist_upd_tag set when newer)
static char g_dist_upd_tag[64] = "", g_dist_have[64] = "";
static void play();

static Setting* dist_setting(const char* key)
{
    for (auto& s : g_set) if (!strcmp(s.key, key)) return &s;
    return nullptr;
}
// adds a choice after the one with value `after` (at the end when missing), keeping the default
static void dist_add_choice(const char* key, const char* after, const char* label, const char* value)
{
    Setting* s = dist_setting(key);
    if (!s) return;
    for (auto& c : s->choices) if (!strcmp(c.value, value)) return;
    const std::string defv = s->choices[s->def].value;
    size_t pos = s->choices.size();
    for (size_t i = 0; i < s->choices.size(); i++) if (!strcmp(s->choices[i].value, after)) pos = i + 1;
    s->choices.insert(s->choices.begin() + pos, Choice{ label, value });
    for (size_t i = 0; i < s->choices.size(); i++) if (defv == s->choices[i].value) s->def = (int)i;
}

// "Check for updates" (PC options). R2 / R3 list it through add_option; R1 appends it after the built-in
// settings (S_UPDATES) and its layout() places the row.
static void dist_register_updates_option()
{
    const char* help = "When the launcher opens, it asks GitHub whether a newer version of the port has been "
                       "released and offers to open the download page. Nothing is downloaded on its own.";
#ifdef DIST_ADD_OPTION
    add_option(SEC_PC, "updates", "Check for updates", help, nullptr, { {"On", "1"}, {"Off", "0"} }, 0);
#else
    g_set.push_back({ "updates", "Check for updates", help, { {"On", "1"}, {"Off", "0"} }, 0 });
#endif
}
#ifndef DIST_ADD_OPTION
static const int S_UPDATES = S_COUNT;
#endif

// Settings and controls are also kept in %USERPROFILE%\Saved Games\<DIST_USER_DIR>\config, so they survive
// a reinstall or an update: missing files are restored from there when the launcher opens, and saved back
// when it closes (run.bat does the same around the game). A portable.txt next to Launcher.exe turns it off.
#ifdef DIST_USER_DIR
static const char* const k_dist_cfg_files[] = { "settings.ini", "settings.bat", "controls.txt", "mouselook.cfg" };
static std::string dist_user_cfg_dir()
{
    if (GetFileAttributesA(path_of("portable.txt").c_str()) != INVALID_FILE_ATTRIBUTES) return "";
    char home[MAX_PATH] = "";
    if (!GetEnvironmentVariableA("USERPROFILE", home, sizeof home) || !home[0]) return "";
    return std::string(home) + "\\Saved Games\\" DIST_USER_DIR "\\config";
}
static void dist_config_restore()
{
    const std::string dir = dist_user_cfg_dir();
    if (dir.empty()) return;
    for (const char* n : k_dist_cfg_files) CopyFileA((dir + "\\" + n).c_str(), path_of(n).c_str(), TRUE);
}
static void dist_config_save()
{
    const std::string dir = dist_user_cfg_dir();
    if (dir.empty()) return;
    const std::string parent = dir.substr(0, dir.rfind('\\'));
    CreateDirectoryA(parent.substr(0, parent.rfind('\\')).c_str(), NULL);   // Saved Games
    CreateDirectoryA(parent.c_str(), NULL);
    CreateDirectoryA(dir.c_str(), NULL);
    for (const char* n : k_dist_cfg_files) {
        const std::string src = path_of(n);
        if (GetFileAttributesA(src.c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileA(src.c_str(), (dir + "\\" + n).c_str(), FALSE);
    }
}
#endif

static void dist_init()
{
#ifdef DIST_USER_DIR
    dist_config_restore();
    atexit(dist_config_save);
#endif
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    g_dist_wine = nt && GetProcAddress(nt, "wine_get_version") != NULL;
    char v[8] = "";
    g_dist_deck = GetEnvironmentVariableA("SteamDeck", v, sizeof v) && v[0] == '1';
    if (g_dist_wine || g_dist_deck) {
        dist_add_choice("fps", "30", "40  (Steam Deck at 40 Hz)", "40");
        dist_add_choice("window", "1280x720", "1280 x 800  (Steam Deck)", "1280x800");
    }
    dist_register_updates_option();
}

static bool dist_updates_on()
{
    Setting* s = dist_setting("updates");
    return !s || strcmp(s->choices[s->sel].value, "0") != 0;
}

// SemVer 2.0 precedence on tags like "v0.2.0-beta.1": 0.2.0-beta < 0.2.0-beta.1 < 0.2.0-rc < 0.2.0.
// Build metadata (+...) is ignored. Returns false when there is no x.y.z core.
struct DistVer { long core[3]; std::string pre; };
static bool dist_parse_ver(const char* s, DistVer* v)
{
    v->core[0] = v->core[1] = v->core[2] = 0;
    v->pre.clear();
    while (*s && !isdigit((unsigned char)*s)) s++;
    if (!*s) return false;
    for (int i = 0; i < 3; i++) {
        v->core[i] = strtol(s, nullptr, 10);
        while (isdigit((unsigned char)*s)) s++;
        if (i < 2) { if (*s != '.' || !isdigit((unsigned char)s[1])) break; s++; }
    }
    if (*s == '-') for (s++; *s && *s != '+' && !isspace((unsigned char)*s); s++) v->pre += *s;
    return true;
}
static int dist_cmp_ident(const std::string& a, const std::string& b)
{
    const bool na = !a.empty() && a.find_first_not_of("0123456789") == std::string::npos;
    const bool nb = !b.empty() && b.find_first_not_of("0123456789") == std::string::npos;
    if (na && nb) {                       // numeric: compare by value without overflow
        const size_t za = a.find_first_not_of('0'), zb = b.find_first_not_of('0');
        const std::string ta = za == std::string::npos ? "" : a.substr(za), tb = zb == std::string::npos ? "" : b.substr(zb);
        if (ta.size() != tb.size()) return ta.size() < tb.size() ? -1 : 1;
        return ta.compare(tb) < 0 ? -1 : ta.compare(tb) > 0;
    }
    if (na != nb) return na ? -1 : 1;     // numeric identifiers sort before alphanumeric ones
    return a.compare(b) < 0 ? -1 : a.compare(b) > 0;
}
static int dist_cmp_ver(const DistVer& a, const DistVer& b)
{
    for (int i = 0; i < 3; i++) if (a.core[i] != b.core[i]) return a.core[i] < b.core[i] ? -1 : 1;
    if (a.pre.empty() || b.pre.empty()) return (int)a.pre.empty() - (int)b.pre.empty();   // a release beats its pre-releases
    size_t pa = 0, pb = 0;
    for (;;) {
        const size_t ea = a.pre.find('.', pa), eb = b.pre.find('.', pb);
        const int c = dist_cmp_ident(a.pre.substr(pa, ea - pa), b.pre.substr(pb, eb - pb));
        if (c) return c;
        if (ea == std::string::npos || eb == std::string::npos) return (ea == std::string::npos) ? (eb == std::string::npos ? 0 : -1) : 1;
        pa = ea + 1; pb = eb + 1;
    }
}
static bool dist_newer(const char* tag, const char* have)
{
    DistVer a, b;
    return dist_parse_ver(tag, &a) && dist_parse_ver(have, &b) && dist_cmp_ver(a, b) > 0;
}
// Newest "tag_name" in a GitHub /releases list (pre-releases included; drafts are not listed for anonymous
// requests). Tags with characters that are not safe in a URL are skipped; escaped quotes in release text never match.
static std::string dist_newest_tag(const std::string& body)
{
    std::string best; DistVer bv{};
    for (size_t p = 0; (p = body.find("\"tag_name\"", p)) != std::string::npos; ) {
        p += 10;
        size_t q = body.find_first_not_of(" \t\r\n", p);
        if (q == std::string::npos || body[q] != ':') continue;
        q = body.find_first_not_of(" \t\r\n", q + 1);
        if (q == std::string::npos || body[q] != '"') continue;
        const size_t e = body.find('"', q + 1);
        if (e == std::string::npos || e - q - 1 >= 64) continue;
        const std::string tag = body.substr(q + 1, e - q - 1);
        if (tag.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz.-+_") != std::string::npos) continue;
        DistVer v;
        if (dist_parse_ver(tag.c_str(), &v) && (best.empty() || dist_cmp_ver(v, bv) > 0)) { best = tag; bv = v; }
    }
    return best;
}

static std::string dist_read_line(const std::string& file)
{
    char line[128] = "";
    if (FILE* f = fopen(file.c_str(), "r")) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
    line[strcspn(line, "\r\n")] = 0;
    return line;
}

static DWORD WINAPI dist_update_thread(LPVOID)
{
    std::string body;
    HINTERNET ses = WinHttpOpen(L"ResistancePorts-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 5000, 5000, 5000, 5000);
        HINTERNET con = WinHttpConnect(ses, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        HINTERNET req = NULL;
        if (con) {
            wchar_t path[200];
            // the full list, not /releases/latest: "latest" never returns a pre-release (the betas are pre-releases)
            swprintf(path, 200, L"/repos/%hs/releases?per_page=30", DIST_REPO);
            req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     WINHTTP_FLAG_SECURE);
        }
        DWORD status = 0, sl = sizeof status;
        if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, NULL, 0, 0, 0) &&
            WinHttpReceiveResponse(req, NULL) &&
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sl,
                                NULL) && status == 200) {
            char buf[4096]; DWORD got = 0;
            while (body.size() < (1u << 20) && WinHttpReadData(req, buf, sizeof buf, &got) && got) body.append(buf, got);
        }
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
    }
    const std::string tag = dist_newest_tag(body);
    if (!tag.empty() && tag.size() < sizeof g_dist_upd_tag && dist_newer(tag.c_str(), g_dist_have) &&
        tag != dist_read_line(path_of("logs\\update_seen.txt")))
        strcpy_s(g_dist_upd_tag, tag.c_str());
    InterlockedExchange(&g_dist_upd_done, 1);
    return 0;
}

static void CALLBACK dist_timer(HWND, UINT, UINT_PTR id, DWORD)
{
    // Steam Deck: A or Start on the controller presses Play
    if (g_dist_deck && g_dist_hw && GetForegroundWindow() == g_dist_hw) {
        typedef DWORD (WINAPI *XGetState)(DWORD, void*);
        static XGetState xget = nullptr; static bool tried = false;
        static WORD prev = 0xFFFF;                 // ignore a button already held when the launcher opened
        if (!tried) {
            tried = true;
            HMODULE x = LoadLibraryA("xinput1_4.dll");
            if (!x) x = LoadLibraryA("xinput9_1_0.dll");
            if (x) xget = (XGetState)(void*)GetProcAddress(x, "XInputGetState");
        }
        struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st = {};
        if (xget && xget(0, &st) == 0) {
            const WORD play_btn = 0x1000 /*A*/ | 0x0010 /*Start*/;
            const WORD pressed = st.buttons & (WORD)~prev;
            prev = st.buttons;
            if (pressed & play_btn) { KillTimer(NULL, id); play(); return; }
        }
    }
    if (g_dist_upd_done == 1) {
        InterlockedExchange(&g_dist_upd_done, 2);
        if (g_dist_upd_tag[0] && g_dist_hw && IsWindow(g_dist_hw)) {
            char msg[512];
            snprintf(msg, sizeof msg,
                     "A new version of the port is available: %s (this copy is %s).\n\n"
                     "Open the download page?\n\n"
                     "(No skips this version. Update checks can be turned off on the Advanced page.)",
                     g_dist_upd_tag, g_dist_have);
            const int r = MessageBoxA(g_dist_hw, msg, "Update available", MB_YESNO | MB_ICONINFORMATION);
            if (r == IDYES) {
                // that release's own page (/releases/latest would skip a pre-release)
                const std::string url = std::string("https://github.com/" DIST_REPO "/releases/tag/") + g_dist_upd_tag;
                ShellExecuteA(g_dist_hw, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
            } else {
                CreateDirectoryA(path_of("logs").c_str(), NULL);
                if (FILE* f = fopen(path_of("logs\\update_seen.txt").c_str(), "w")) { fprintf(f, "%s\n", g_dist_upd_tag); fclose(f); }
            }
        }
    }
    if (!g_dist_deck && g_dist_upd_done == 2) KillTimer(NULL, id);
}

static void dist_start(HWND hw)
{
    g_dist_hw = hw;
    const std::string have = dist_read_line(path_of("VERSION"));
    strcpy_s(g_dist_have, have.c_str());
    const bool check = dist_updates_on() && g_dist_have[0] && !getenv("PORT_NO_UPDATE_CHECK");
    if (check) {
        if (HANDLE t = CreateThread(NULL, 0, dist_update_thread, NULL, 0, NULL)) CloseHandle(t);
    } else {
        g_dist_upd_done = 2;
    }
    if (check || g_dist_deck) SetTimer(NULL, 0, 100, dist_timer);
}
