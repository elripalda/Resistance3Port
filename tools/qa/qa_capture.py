#!/usr/bin/env python3
"""Boot the game to fixed spots, capture frames, compare with the stored references.

  python tools\\qa\\qa_capture.py                      boot, capture every scene, compare -> report.html
  python tools\\qa\\qa_capture.py --make-reference     same boot, store the captures as the new references
  python tools\\qa\\qa_capture.py --scenes heli_load   only some scenes (the boot still runs the whole script)
  python tools\\qa\\qa_capture.py --compare-only DIR   re-compare an earlier run's frames
Options: --exe <path> (default: first of qa_game.json "exe" that exists), --env K=V (repeatable), --keep-running.

Scenes and the boot script are in tools\\qa\\qa_game.json. Runs go to tools\\qa\\runs\\capture_<time>\\,
references live in tools\\qa\\refs\\<game>\\ (thresholds.json / <scene>.mask.png there tune the comparison).
"""
import argparse
import os
import subprocess
import sys

import qa_lib as q


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--make-reference", action="store_true")
    ap.add_argument("--scenes", default="")
    ap.add_argument("--exe")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--profile")
    ap.add_argument("--keep-running", action="store_true")
    ap.add_argument("--compare-only", metavar="FRAMES_DIR")
    a = ap.parse_args()

    prof = q.load_profile(a.profile)
    ref_dir = os.path.join(q.QA_DIR, "refs", prof["game"])
    scenes = [s for s in a.scenes.split(",") if s]

    if a.compare_only:
        cap_dir = os.path.abspath(a.compare_only)
        run_dir = os.path.dirname(cap_dir)
    else:
        run_dir = os.path.join(q.QA_DIR, "runs", f"capture_{q.stamp()}")
        cap_dir = os.path.join(run_dir, "frames")
        os.makedirs(cap_dir, exist_ok=True)
        extra = dict(prof.get("capture_env", {}))
        extra["R2_QA_CAPTURE"] = cap_dir
        for kv in a.env:
            k, _, v = kv.partition("=")
            extra[k] = v
        game = q.Game(prof, run_dir, a.exe, extra)
        game.start()
        try:
            ok = q.run_steps(game, prof["capture_steps"], cap_dir, scenes)
        finally:
            if not a.keep_running:
                game.stop()
        if not ok:
            q.log(f"the game died (exit code {game.exit_code()}) - see {run_dir}")

    cmp = os.path.join(q.QA_DIR, "qa_compare.py")
    if a.make_reference:
        r = subprocess.call([sys.executable, cmp, "make-reference", "--cur", cap_dir, "--ref", ref_dir]
                            + (["--scenes", ",".join(scenes)] if scenes else []))
        q.log(f"references in {ref_dir}")
        return r
    r = subprocess.call([sys.executable, cmp, "compare", "--cur", cap_dir, "--ref", ref_dir,
                         "--out", os.path.join(run_dir, "report")]
                        + (["--scenes", ",".join(scenes)] if scenes else []))
    q.log({0: "PASS", 1: "REGRESSIONS FOUND", 2: "MISSING captures or references"}.get(r, f"exit {r}"))
    return r


if __name__ == "__main__":
    sys.exit(main())
