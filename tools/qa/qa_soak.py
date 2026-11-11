#!/usr/bin/env python3
"""Soak test: boot into gameplay, play with scripted pad input for N minutes, watch for trouble.

  python tools\\qa\\qa_soak.py --minutes 30
  python tools\\qa\\qa_soak.py --minutes 60 --env R2_FIFO_SPU_LOCK=1 --tag spulock
Options: --exe, --env K=V (repeatable), --sample 30 (seconds), --no-input, --no-stop-on-crash, --warmup 120.

Every --sample seconds one CSV row: fps (from the game's own 5 s profiler lines), working set / private
bytes, handles, threads, process CPU and the busiest threads, plus running counts of FIFO recoveries,
invalid RSX methods, semaphore timeouts, FIFO trace dumps, hang/crash dumps. Hangs are detected from
the hang-watchdog dumps, a "not responding" window, and missing fps lines. Output in
tools\\qa\\runs\\soak_<time>_<tag>\\: soak.csv, summary.txt, summary.json, the game logs.
"""
import argparse
import csv
import glob
import json
import os
import re
import sys
import time

import psutil

import qa_lib as q


class Tail:
    def __init__(self, path, from_end=False):
        self.path, self.pos = path, None
        if from_end and os.path.isfile(path):
            self.pos = os.path.getsize(path)

    def read(self):
        try:
            sz = os.path.getsize(self.path)
        except OSError:
            return ""
        if self.pos is None or sz < self.pos:
            self.pos = 0
        if sz == self.pos:
            return ""
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read(min(sz - self.pos, 64 << 20))
        self.pos += len(data)
        return data.decode("utf-8", "ignore")


def slope_per_min(ts, vs):
    if len(ts) < 3:
        return 0.0
    n = len(ts)
    mt, mv = sum(ts) / n, sum(vs) / n
    den = sum((t - mt) ** 2 for t in ts)
    return (sum((t - mt) * (v - mv) for t, v in zip(ts, vs)) / den * 60.0) if den else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--sample", type=float, default=30)
    ap.add_argument("--exe")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--tag", default="")
    ap.add_argument("--profile")
    ap.add_argument("--no-input", action="store_true")
    ap.add_argument("--no-stop-on-crash", action="store_true")
    ap.add_argument("--warmup", type=float, default=120, help="seconds of play ignored by the memory-growth fit")
    a = ap.parse_args()

    prof = q.load_profile(a.profile)
    sk = prof["soak"]
    run_dir = os.path.join(q.QA_DIR, "runs", f"soak_{q.stamp()}" + (f"_{a.tag}" if a.tag else ""))
    os.makedirs(run_dir, exist_ok=True)
    extra = dict(sk.get("env", {}))
    for kv in a.env:
        k, _, v = kv.partition("=")
        extra[k] = v
    game = q.Game(prof, run_dir, a.exe, extra)

    crash_dir = os.path.join(q.ROOT, "logs", "crash")
    trace_dir = q.expand(extra.get("R2_FIFO_TRACE_DIR", os.path.join(q.ROOT, "logs", "fifo_trace")), run_dir)
    before = {
        "crash": set(glob.glob(os.path.join(crash_dir, "*_crash.dmp"))),
        "hang": set(glob.glob(os.path.join(crash_dir, "*_hang.dmp"))),
        "trace": set(glob.glob(os.path.join(trace_dir, "fifo_desync_*.txt"))),
    }
    crashes_txt = Tail(os.path.join(crash_dir, "crashes.txt"), from_end=True)

    # "name": "regex" or {"rx": "regex", "in": "<log file name>"} (count a line only from that log)
    pats, pat_in = {}, {}
    for k, v in sk["patterns"].items():
        pats[k] = re.compile(v["rx"] if isinstance(v, dict) else v)
        pat_in[k] = v.get("in", "").lower() if isinstance(v, dict) else ""
    counts = {k: 0 for k in pats}
    first_hits = {k: [] for k in pats}
    fps_re = re.compile(prof["fps_regex"])
    # logs under {root} survive between runs: read only what this run appends (they restart at 0 when truncated)
    tails = [Tail(q.expand(p, run_dir), from_end=p.startswith("{root}")) for p in prof["logs"]]

    game.start()
    t_start = time.time()
    state = {"fps": [], "last_fps_t": time.time()}

    def scan():
        for t in tails:
            base = os.path.basename(t.path).lower()
            txt = t.read()
            if not txt:
                continue
            for m in fps_re.finditer(txt):
                state["fps"].append(float(m.group(1)))
                state["last_fps_t"] = time.time()
            for line in txt.splitlines():
                for k, rx in pats.items():
                    if (not pat_in[k] or pat_in[k] == base) and rx.search(line):
                        counts[k] += 1
                        if len(first_hits[k]) < 5:
                            first_hits[k].append(f"+{time.time() - t_start:.0f}s {line.strip()[:240]}")

    q.log("booting into gameplay")
    if not q.run_steps(game, prof["soak_boot_steps"], on_tick=scan):
        q.log("game died while booting")
    t_play = time.time()
    end = t_play + a.minutes * 60
    rows, events = [], []
    inputs = sk["inputs"]
    ii = 0
    next_sample = time.time()
    next_input = time.time()
    last_cpu, last_thr, last_t, last_pid = None, {}, None, None
    hung_samples = 0
    crash_text = ""
    verdict_reasons = []

    fields = ["t_s", "play_s", "fps_avg", "fps_min", "ws_mb", "private_mb", "handles", "threads", "cpu_pct",
              "top_threads", "responding"] + list(pats) + ["trace_dumps", "hang_dumps", "crash_dumps", "alive"]
    fcsv = open(os.path.join(run_dir, "soak.csv"), "w", newline="")
    w = csv.DictWriter(fcsv, fieldnames=fields)
    w.writeheader()

    while time.time() < end:
        scan()
        now = time.time()
        if not game.alive():
            code = game.exit_code()
            events.append(f"+{now - t_start:.0f}s game process ended (exit code {code})")
            verdict_reasons.append(f"process ended (exit code {code})")
            break
        if not a.no_input and now >= next_input:
            cmd = inputs[ii % len(inputs)]
            ii += 1
            game.console(cmd)
            last = cmd.split()[-1]
            hold = int(last) / 1000.0 if last.isdigit() else 0.5
            next_input = time.time() + hold + 0.6
        if now >= next_sample:
            next_sample = now + a.sample
            gp = game.game_process()
            row = {"t_s": round(now - t_start), "play_s": round(now - t_play), "alive": int(gp is not None)}
            fps = state["fps"]
            row["fps_avg"] = round(sum(fps) / len(fps), 1) if fps else ""
            row["fps_min"] = round(min(fps), 1) if fps else ""
            state["fps"] = []
            if gp:
                try:
                    with gp.oneshot():
                        mi = gp.memory_info()
                        row["ws_mb"] = round(mi.rss / 2 ** 20, 1)
                        row["private_mb"] = round(getattr(mi, "private", mi.vms) / 2 ** 20, 1)
                        row["handles"] = gp.num_handles()
                        row["threads"] = gp.num_threads()
                        ct = gp.cpu_times()
                        cpu = ct.user + ct.system
                        thr = {t.id: t.user_time + t.system_time for t in gp.threads()}
                    if last_cpu is not None and last_t and gp.pid == last_pid:
                        dt = now - last_t
                        row["cpu_pct"] = round((cpu - last_cpu) / dt * 100.0, 1)  # 100 = one core
                        busiest = sorted(((thr[i] - last_thr.get(i, 0.0)) / dt * 100.0, i) for i in thr)[-4:]
                        row["top_threads"] = " ".join(f"{i}:{p:.0f}%" for p, i in reversed(busiest))
                    last_cpu, last_thr, last_t, last_pid = cpu, thr, now, gp.pid
                    hung = q.is_hung(gp.pid)
                    row["responding"] = int(not hung)
                    hung_samples = hung_samples + 1 if hung else 0
                except psutil.Error:
                    pass
            for k in pats:
                row[k] = counts[k]
            new_trace = set(glob.glob(os.path.join(trace_dir, "fifo_desync_*.txt"))) - before["trace"]
            new_hang = set(glob.glob(os.path.join(crash_dir, "*_hang.dmp"))) - before["hang"]
            new_crash = set(glob.glob(os.path.join(crash_dir, "*_crash.dmp"))) - before["crash"]
            row.update(trace_dumps=len(new_trace), hang_dumps=len(new_hang), crash_dumps=len(new_crash))
            crash_text += crashes_txt.read()
            w.writerow(row)
            fcsv.flush()
            rows.append(row)
            q.log(" ".join(f"{k}={row.get(k, '')}" for k in ("play_s", "fps_avg", "private_mb", "handles", "threads", "cpu_pct"))
                  + "".join(f" {k}={counts[k]}" for k in pats if counts[k]))
            stale = now - state["last_fps_t"]
            if hung_samples >= 2 or (stale > 60 and now - t_play > 60):
                events.append(f"+{now - t_start:.0f}s HANG suspected (window not responding x{hung_samples}, no fps line for {stale:.0f}s)")
                if "hang suspected" not in verdict_reasons:
                    verdict_reasons.append("hang suspected")
            if new_crash and not a.no_stop_on_crash:
                verdict_reasons.append("crash dump written")
                break
        time.sleep(0.25)

    scan()
    game.stop()
    fcsv.close()

    # ---- summary
    play_rows = [r for r in rows if r["play_s"] >= a.warmup and r.get("private_mb") not in (None, "")]
    ts = [r["play_s"] for r in play_rows]
    mem_slope = slope_per_min(ts, [r["private_mb"] for r in play_rows])
    ws_slope = slope_per_min(ts, [r["ws_mb"] for r in play_rows])
    h_slope = slope_per_min(ts, [r["handles"] for r in play_rows])
    fps_vals = [r["fps_avg"] for r in rows if r.get("fps_avg") not in (None, "")]
    final = rows[-1] if rows else {}
    th = sk.get("thresholds", {})
    if mem_slope > th.get("mem_mb_per_min", 4.0) and play_rows and \
            play_rows[-1]["private_mb"] - play_rows[0]["private_mb"] > th.get("mem_growth_mb", 150):
        verdict_reasons.append(f"memory growth {mem_slope:.1f} MB/min")
    if h_slope > th.get("handles_per_min", 20):
        verdict_reasons.append(f"handle growth {h_slope:.0f}/min")
    for k in pats:
        if counts[k] and k in th.get("fail_on", list(pats)):
            verdict_reasons.append(f"{k} x{counts[k]}")
    if final.get("hang_dumps"):
        verdict_reasons.append(f"hang dumps x{final['hang_dumps']}")
    if final.get("crash_dumps") and "crash dump written" not in verdict_reasons:
        verdict_reasons.append(f"crash dumps x{final['crash_dumps']}")
    verdict = "PASS" if not verdict_reasons else "FAIL"
    new_traces = sorted(set(glob.glob(os.path.join(trace_dir, "fifo_desync_*.txt"))) - before["trace"])
    new_dumps = sorted(set(glob.glob(os.path.join(crash_dir, "*.dmp"))) - before["crash"] - before["hang"])
    summary = {
        "verdict": verdict, "reasons": verdict_reasons, "game": prof["game"], "exe": game.exe,
        "env": extra, "minutes_planned": a.minutes, "played_s": round(time.time() - t_play),
        "fps_avg": round(sum(fps_vals) / len(fps_vals), 1) if fps_vals else None,
        "fps_worst_sample": min(fps_vals) if fps_vals else None,
        "private_mb_first": play_rows[0]["private_mb"] if play_rows else None,
        "private_mb_last": play_rows[-1]["private_mb"] if play_rows else None,
        "private_mb_per_min": round(mem_slope, 2), "ws_mb_per_min": round(ws_slope, 2),
        "handles_per_min": round(h_slope, 1), "counts": counts, "first_hits": first_hits,
        "events": events, "fifo_trace_dumps": new_traces, "dumps": new_dumps,
        "crashes_txt_new": crash_text.strip().splitlines(),
    }
    with open(os.path.join(run_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    lines = [f"SOAK {verdict}  {prof['game']}  {q.rel(game.exe)}  {' '.join(a.env)}",
             f"played {summary['played_s'] / 60:.1f} of {a.minutes:g} min; fps avg {summary['fps_avg']}, worst 30 s sample {summary['fps_worst_sample']}",
             f"private bytes {summary['private_mb_first']} -> {summary['private_mb_last']} MB ({mem_slope:+.2f} MB/min after {a.warmup:.0f} s warm-up), "
             f"working set {ws_slope:+.2f} MB/min, handles {h_slope:+.1f}/min"]
    if verdict_reasons:
        lines.append("reasons: " + "; ".join(verdict_reasons))
    for k in pats:
        if counts[k]:
            lines.append(f"{k}: {counts[k]}")
            lines += ["    " + x for x in first_hits[k]]
    lines += events
    lines += [f"FIFO trace dump: {p}" for p in new_traces]
    lines += [f"dump: {p}" for p in new_dumps]
    lines += [f"crashes.txt: {x}" for x in summary["crashes_txt_new"]]
    lines.append(f"csv: {os.path.join(run_dir, 'soak.csv')}")
    with open(os.path.join(run_dir, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
