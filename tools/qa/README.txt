QA tools (visual regression, soak, crash reports). Needs Python 3 + numpy, Pillow, psutil
(pip install numpy pillow psutil). Run from the repo root. Profile: tools\qa\qa_game.json
(exe candidates, env, boot steps, capture scenes, soak inputs, log patterns, thresholds).

Visual regression
  python tools\qa\qa_capture.py --make-reference   boot, capture every scene -> tools\qa\refs\<game>\
  python tools\qa\qa_capture.py                    boot, capture, compare -> runs\capture_<time>\report\report.html
  python tools\qa\qa_capture.py --scenes a,b       only those scenes
  python tools\qa\qa_capture.py --compare-only tools\qa\runs\capture_<time>\frames
  python tools\qa\qa_compare.py compare --cur DIR --ref DIR [--out DIR]   (exit 0 pass, 1 regressions, 2 missing)
  python tools\qa\qa_compare.py selftest            synthetic shadow/colour/texture/flat regressions
  Tuning (in the refs dir): thresholds.json ({"ssim_tile": 0.80, ...} or {"scenes": {"heli_load": {...}}}),
  <scene>.mask.png (white = compare, black = ignore, e.g. HUD timers / particles).
  Steps: "wait S" | "pad BITS WAIT [lx ly rx ry ms]" | "console WAIT text" | "capture NAME [frames] [every_n_flips]".

Soak
  python tools\qa\qa_soak.py --minutes 30 [--tag name] [--env K=V ...] [--no-input] [--sample 30]
  -> runs\soak_<time>_<tag>\soak.csv, summary.txt, summary.json, game logs. Exit 0 pass, 1 problems found.

Crash notice
  project\launcher_crash.inc (one #include + one crashnote_start() call in launcher.cpp).
  tools\qa\build_qa_launcher.bat -> build_qa\Launcher_qa.exe (Launcher.exe untouched).
R3 specifics
  Capture uses the renderer hook (R2_QA_CAPTURE, rpcs3_shim\r2_qa_capture.cpp); needs build_qa\Resistance3.exe:
  tools\qa\build_qa_rpcs3.bat, then tools\qa\build_qa_exe.bat. With the live build\Resistance3.exe it falls back to
  PrintWindow capture. Boot steps are the scratch\r3bench.ps1 route to Haven.
