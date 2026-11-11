/* Resistance PC ports: "Report a problem" -- packs what a developer needs to
 * look at a crash, hang or glitch into one zip on the tester's Desktop
 * (shared by the R1/R2/R3 ports -- keep the three copies identical).
 *
 * The launcher is one translation unit: #include "port_report.cpp" (which
 * includes this header). Nothing is uploaded anywhere.
 *
 *   port_report_start(main_window, exe_dir)
 *       once the launcher window is shown: when the game crashed or hung since
 *       the last time (a dump in logs\crash newer than logs\crash\.seen), asks
 *       "The game crashed last time -- create a report?"; checks again whenever
 *       the launcher is in front (a 3 s timer with its own TIMERPROC).
 *   port_report_ui(main_window)
 *       the "Report a problem" button: description box, "include my save
 *       files" tick box, creates Desktop\<Game>_report_<time>.zip and shows it
 *       in Explorer.
 *   port_report::create(options, result)
 *       the bundler itself, no UI (tests, tools).
 */
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace port_report {

struct Options {
    std::string root;            /* the game folder (run.bat, logs\, build\) */
    std::wstring out_dir;        /* where the zip goes; empty = the Desktop */
    std::string game;            /* zip name prefix; empty = detected (Resistance3, ...) */
    std::string note;            /* the tester's description, saved as note.txt */
    bool include_saves = false;
    uint64_t cap_bytes = 50ull << 20;   /* total size of everything packed (before compression) */
};

struct Result {
    std::wstring zip;            /* the zip written */
    uint64_t packed_bytes = 0;   /* input bytes packed */
    int files = 0;
    std::vector<std::string> skipped;   /* "name (why)" */
    bool compressed = false;     /* tar.exe deflate; false = stored-zip fallback */
    std::string error;
};

bool create(const Options& o, Result& r);
std::string detect_game(const std::string& root);
std::string find_root(const char* exe_dir);
std::vector<std::string> save_dirs(const std::string& root, const std::string& game);
uint64_t dir_size(const std::string& dir, uint64_t stop_after = ~0ull);
std::string system_info(const std::string& root);

} // namespace port_report

void port_report_start(HWND owner, const char* exe_dir);
void port_report_ui(HWND owner);

#endif /* _WIN32 */
