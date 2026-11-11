#!/usr/bin/env python3
"""Perceptual screenshot regression check for the Resistance ports.

Captures are PNGs named <scene>_<NN>.png (NN = frame index) in one directory.
A reference set is the same layout under tools\\qa\\refs\\<game>\\.

  python qa_compare.py compare  --cur <capture dir> --ref <ref dir> [--out <report dir>]
  python qa_compare.py make-reference --cur <capture dir> --ref <ref dir> [--scenes a,b]
  python qa_compare.py selftest

Per scene each captured frame is matched to its best reference frame (tolerates
a frame or two of timing jitter), then compared tile by tile (16x9 grid):
  * SSIM on luma (Gaussian window), mean over the tile
  * mean luma delta          -> "brighter" (missing shadow/AO/fog) / "darker" (missing light, black geometry)
  * chromaticity delta      -> "colour shift / corruption" (swizzled channels, NaN magenta, wrong format)
  * detail (gradient energy) -> "detail lost" (flat/blurred/missing texture) / "detail added" (garbage, noise)
  * low SSIM with similar means -> "structure / texture changed" (texture swap, wrong UVs, moved geometry)
A tile is flagged only if it fails in the best-matching frame. Optional per-scene
ignore masks: <ref dir>\\<scene>.mask.png (white = ignore), e.g. for HUD timers.
Exit code: 0 = pass, 1 = regressions, 2 = missing references/captures.
Needs numpy + Pillow only.
"""
import argparse
import datetime
import html
import json
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw

GRID_X, GRID_Y = 16, 9
DEFAULTS = {
    "ssim_tile": 0.80,      # tile SSIM below this is suspicious
    "ssim_frame": 0.90,     # whole-frame mean SSIM below this fails the scene
    "luma_delta": 18.0,     # mean luma change (0..255) in a tile
    "chroma_delta": 6.0,    # mean chromaticity change |dr|+|dg| (x100) of lit pixels in a tile
    "detail_ratio": 0.45,   # gradient energy ratio (cur/ref) below this, or above 1/this
    "min_bad_tiles": 1,     # scene fails with at least this many flagged tiles
    "work_width": 960,      # images are compared at this width (noise / AA tolerance)
}
NAME_RE = re.compile(r"^(?P<scene>.+?)_(?P<idx>\d+)\.png$", re.I)


# ---------------------------------------------------------------- image utils
def load_rgb(path, width=None):
    im = Image.open(path).convert("RGB")
    if width and im.width != width:
        h = max(1, round(im.height * width / im.width))
        im = im.resize((width, h), Image.BILINEAR if width > im.width else Image.BOX)
    return np.asarray(im, dtype=np.float32)


def ycbcr(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    return y, cb, cr


def gauss_blur(a, sigma=1.5):
    rad = int(3 * sigma + 0.5)
    x = np.arange(-rad, rad + 1, dtype=np.float32)
    k = np.exp(-(x * x) / (2 * sigma * sigma))
    k /= k.sum()
    p = np.pad(a, rad, mode="reflect")
    t = np.zeros((p.shape[0], a.shape[1]), np.float32)
    for i, w in enumerate(k):
        t += w * p[:, i:i + a.shape[1]]
    o = np.zeros_like(a)
    for i, w in enumerate(k):
        o += w * t[i:i + a.shape[0], :]
    return o


def ssim_map(a, b):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    mu_a, mu_b = gauss_blur(a), gauss_blur(b)
    saa = gauss_blur(a * a) - mu_a * mu_a
    sbb = gauss_blur(b * b) - mu_b * mu_b
    sab = gauss_blur(a * b) - mu_a * mu_b
    return ((2 * mu_a * mu_b + c1) * (2 * sab + c2)) / ((mu_a ** 2 + mu_b ** 2 + c1) * (saa + sbb + c2))


def grad_energy(y):
    gx = np.abs(np.diff(y, axis=1, append=y[:, -1:]))
    gy = np.abs(np.diff(y, axis=0, append=y[-1:, :]))
    return gx + gy


def tiles(h, w):
    for ty in range(GRID_Y):
        for tx in range(GRID_X):
            y0, y1 = ty * h // GRID_Y, (ty + 1) * h // GRID_Y
            x0, x1 = tx * w // GRID_X, (tx + 1) * w // GRID_X
            yield tx, ty, y0, y1, x0, x1


# ---------------------------------------------------------------- comparison
def compare_pair(ref, cur, mask, th):
    """ref/cur: float32 HxWx3 same size. mask: bool HxW (True = ignore) or None."""
    yr = ycbcr(ref)[0]
    yc = ycbcr(cur)[0]
    smap = ssim_map(yr, yc)
    ger, gec = gauss_blur(grad_energy(yr), 2.0), gauss_blur(grad_energy(yc), 2.0)
    h, w = yr.shape
    out = []
    valid = np.ones((h, w), bool) if mask is None else ~mask
    for tx, ty, y0, y1, x0, x1 in tiles(h, w):
        v = valid[y0:y1, x0:x1]
        if v.mean() < 0.5:
            out.append(dict(tx=tx, ty=ty, ignored=True))
            continue
        s = float(smap[y0:y1, x0:x1][v].mean())
        dl = float(yc[y0:y1, x0:x1][v].mean() - yr[y0:y1, x0:x1][v].mean())
        # chromaticity (brightness-invariant, so a lost shadow is not also a "colour shift"); dark pixels don't vote
        sr, sc = ref[y0:y1, x0:x1].sum(-1), cur[y0:y1, x0:x1].sum(-1)
        lit = v & (sr > 40) & (sc > 40)
        if lit.mean() > 0.2:
            nr = ref[y0:y1, x0:x1][lit] / sr[lit][:, None]
            nc = cur[y0:y1, x0:x1][lit] / sc[lit][:, None]
            dch = float(np.abs(nc - nr)[:, :2].sum(-1).mean() * 100.0)
        else:
            dch = 0.0
        er, ec = float(ger[y0:y1, x0:x1][v].mean()), float(gec[y0:y1, x0:x1][v].mean())
        ratio = (ec + 1.0) / (er + 1.0)
        reasons = []
        if abs(dl) > th["luma_delta"]:
            reasons.append("brighter (missing shadow/AO/fog?)" if dl > 0 else "darker (missing light / black geometry?)")
        if dch > th["chroma_delta"]:
            reasons.append("colour shift / corruption")
        if ratio < th["detail_ratio"] and er > 2.0:
            reasons.append("detail lost (flat / blurred / missing texture)")
        elif ratio > 1.0 / th["detail_ratio"] and ec > 2.0:
            reasons.append("detail added (garbage / noise / wrong texture)")
        if s < th["ssim_tile"] and not reasons:
            reasons.append("structure / texture changed")
        bad = s < th["ssim_tile"] and bool(reasons) or abs(dl) > th["luma_delta"] * 1.5 or dch > th["chroma_delta"] * 1.5
        out.append(dict(tx=tx, ty=ty, ssim=s, dluma=dl, dchroma=dch, detail=ratio, bad=bool(bad), reasons=reasons if bad else []))
    used = smap[valid] if valid.any() else smap.ravel()
    return float(used.mean()), out, smap


def heatmap(smap, size):
    v = np.clip((1.0 - smap) * 4.0, 0, 1)  # SSIM 1 -> black, <=0.75 -> full
    rgb = np.stack([v * 255, (v ** 2) * 160, (1 - v) * 40 * (v > 0.02)], -1).astype(np.uint8)
    return Image.fromarray(rgb).resize(size, Image.NEAREST)


def annotate(cur_img, tile_res, w, h):
    im = cur_img.copy()
    d = ImageDraw.Draw(im)
    for t in tile_res:
        x0, x1 = t["tx"] * w // GRID_X, (t["tx"] + 1) * w // GRID_X - 1
        y0, y1 = t["ty"] * h // GRID_Y, (t["ty"] + 1) * h // GRID_Y - 1
        if t.get("ignored"):
            d.rectangle([x0, y0, x1, y1], outline=(80, 80, 255))
        elif t.get("bad"):
            col = (255, 40, 40)
            r = " ".join(t["reasons"])
            if "colour" in r: col = (255, 0, 255)
            elif "brighter" in r: col = (255, 220, 0)
            elif "darker" in r: col = (0, 200, 255)
            d.rectangle([x0, y0, x1, y1], outline=col, width=3)
    return im


def group(dirpath):
    scenes = {}
    if not os.path.isdir(dirpath):
        return scenes
    for f in sorted(os.listdir(dirpath)):
        m = NAME_RE.match(f)
        if m and not f.lower().endswith(".mask.png"):
            scenes.setdefault(m.group("scene"), []).append(os.path.join(dirpath, f))
    return scenes


def load_thresholds(ref_dir, overrides):
    th = dict(DEFAULTS)
    p = os.path.join(ref_dir, "thresholds.json")
    if os.path.isfile(p):
        with open(p) as f:
            data = json.load(f)
        th.update({k: v for k, v in data.items() if not isinstance(v, dict)})
        th["_scene"] = {k: v for k, v in data.items() if isinstance(v, dict)}
    for k, v in overrides.items():
        if v is not None:
            th[k] = v
    return th


def scene_thresholds(th, scene):
    t = dict(th)
    t.update(th.get("_scene", {}).get(scene, {}))
    return t


def compare_dirs(cur_dir, ref_dir, out_dir, overrides, only=None):
    th_all = load_thresholds(ref_dir, overrides)
    W = int(th_all["work_width"])
    refs, curs = group(ref_dir), group(cur_dir)
    os.makedirs(os.path.join(out_dir, "img"), exist_ok=True)
    results = []
    for scene in sorted(set(refs) | set(curs)):
        if only and scene not in only:
            continue
        th = scene_thresholds(th_all, scene)
        if scene not in refs or scene not in curs:
            results.append(dict(scene=scene, status="MISSING",
                                note="no reference" if scene not in refs else "not captured"))
            continue
        ref_imgs = [load_rgb(p, W) for p in refs[scene]]
        mask = None
        mp = os.path.join(ref_dir, scene + ".mask.png")
        if os.path.isfile(mp):
            mi = Image.open(mp).convert("L").resize((ref_imgs[0].shape[1], ref_imgs[0].shape[0]), Image.NEAREST)
            mask = np.asarray(mi) > 127
        frames = []
        for cp in curs[scene]:
            cur = load_rgb(cp, W)
            best = None
            for ri, ref in enumerate(ref_imgs):
                if ref.shape != cur.shape:
                    cur_r = np.asarray(Image.fromarray(cur.astype(np.uint8)).resize((ref.shape[1], ref.shape[0]), Image.BILINEAR), np.float32)
                else:
                    cur_r = cur
                score, tres, smap = compare_pair(ref, cur_r, mask, th)
                nbad = sum(1 for t in tres if t.get("bad"))
                key = (nbad, -score)
                if best is None or key < best[0]:
                    best = (key, ri, score, tres, smap, cur_r)
            frames.append((cp, best))
        # the scene verdict comes from its best frame; flagged regions must also appear in most frames
        frames.sort(key=lambda fr: fr[1][0])
        cp, (key, ri, score, tres, smap, cur_r) = frames[0]
        nbad = key[0]
        persistent = []
        for t in tres:
            if t.get("bad"):
                hits = sum(1 for _, b in frames if any(u.get("bad") and u["tx"] == t["tx"] and u["ty"] == t["ty"] for u in b[3]))
                t["frames_hit"] = hits
                persistent.append(t)
        fail = nbad >= th["min_bad_tiles"] or score < th["ssim_frame"]
        status = "FAIL" if fail else ("WARN" if any(b[0][0] for _, b in frames[1:]) else "PASS")
        h, w = cur_r.shape[:2]
        tag = re.sub(r"[^\w.-]", "_", scene)
        ref_im = Image.fromarray(ref_imgs[ri].astype(np.uint8))
        cur_im = Image.fromarray(cur_r.astype(np.uint8))
        ref_im.save(os.path.join(out_dir, "img", f"{tag}_ref.jpg"), quality=90)
        cur_im.save(os.path.join(out_dir, "img", f"{tag}_cur.jpg"), quality=90)
        heatmap(smap, (w, h)).save(os.path.join(out_dir, "img", f"{tag}_ssim.png"))
        annotate(cur_im, tres, w, h).save(os.path.join(out_dir, "img", f"{tag}_flags.jpg"), quality=90)
        diff = np.clip(np.abs(cur_r - ref_imgs[ri]) * 4, 0, 255).astype(np.uint8)
        Image.fromarray(diff).save(os.path.join(out_dir, "img", f"{tag}_absdiff.jpg"), quality=90)
        results.append(dict(scene=scene, status=status, ssim=score, bad_tiles=nbad, tag=tag,
                            cur=os.path.basename(cp), ref=os.path.basename(refs[scene][ri]),
                            frames=[dict(file=os.path.basename(p), bad=b[0][0], ssim=b[2]) for p, b in frames],
                            flags=persistent, frames_total=len(frames)))
    write_report(results, cur_dir, ref_dir, out_dir, th_all)
    with open(os.path.join(out_dir, "results.json"), "w") as f:
        json.dump(results, f, indent=1)
    return results


def write_report(results, cur_dir, ref_dir, out_dir, th):
    e = html.escape
    col = {"PASS": "#2a2", "WARN": "#c90", "FAIL": "#d22", "MISSING": "#888"}
    rows = []
    for r in results:
        rows.append(f'<tr><td><a href="#{e(r.get("tag", r["scene"]))}">{e(r["scene"])}</a></td>'
                    f'<td style="color:{col[r["status"]]};font-weight:bold">{r["status"]}</td>'
                    f'<td>{r.get("ssim", 0):.4f}</td><td>{r.get("bad_tiles", "")}</td><td>{e(r.get("note", ""))}</td></tr>')
    body = []
    for r in results:
        if r["status"] == "MISSING":
            continue
        t = r["tag"]
        flags = "".join(
            f'<li>tile ({f["tx"]},{f["ty"]}): {e(", ".join(f["reasons"]))} - SSIM {f["ssim"]:.3f}, '
            f'dLuma {f["dluma"]:+.1f}, dChroma {f["dchroma"]:.1f}, detail x{f["detail"]:.2f}, '
            f'in {f["frames_hit"]}/{r["frames_total"]} frames</li>' for f in r["flags"]) or "<li>none</li>"
        fr = ", ".join(f'{e(x["file"])} ({x["ssim"]:.3f}{", " + str(x["bad"]) + " bad" if x["bad"] else ""})' for x in r["frames"])
        body.append(f'''<h2 id="{e(t)}">{e(r["scene"])} <span style="color:{col[r["status"]]}">{r["status"]}</span></h2>
<p>best frame {e(r["cur"])} vs reference {e(r["ref"])} - SSIM {r["ssim"]:.4f}, flagged tiles {r["bad_tiles"]}<br>
<small>all frames: {fr}</small></p>
<div class="row"><figure><img src="img/{t}_ref.jpg"><figcaption>reference</figcaption></figure>
<figure><img src="img/{t}_cur.jpg" class="swap" data-alt="img/{t}_ref.jpg"><figcaption>current (hover = reference)</figcaption></figure></div>
<div class="row"><figure><img src="img/{t}_flags.jpg"><figcaption>flags: red structure, magenta colour, yellow brighter, cyan darker, blue ignored</figcaption></figure>
<figure><img src="img/{t}_ssim.png"><figcaption>1-SSIM heatmap</figcaption></figure>
<figure><img src="img/{t}_absdiff.jpg"><figcaption>|diff| x4</figcaption></figure></div>
<ul>{flags}</ul><hr>''')
    thr = ", ".join(f"{k}={v}" for k, v in th.items() if not k.startswith("_"))
    doc = f'''<!doctype html><html><head><meta charset="utf-8"><title>QA visual report</title><style>
body{{background:#181818;color:#ddd;font-family:Segoe UI,sans-serif;margin:20px}} a{{color:#8cf}}
table{{border-collapse:collapse}} td,th{{border:1px solid #444;padding:3px 10px}}
.row{{display:flex;gap:8px;flex-wrap:wrap}} figure{{margin:0}} img{{max-width:640px;width:100%;border:1px solid #333}}
figcaption{{font-size:12px;color:#999}}</style></head><body>
<h1>Visual regression report</h1>
<p>{e(datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))}<br>capture: {e(os.path.abspath(cur_dir))}<br>
reference: {e(os.path.abspath(ref_dir))}<br><small>thresholds: {e(thr)}</small></p>
<table><tr><th>scene</th><th>status</th><th>SSIM</th><th>bad tiles</th><th>note</th></tr>{"".join(rows)}</table><hr>
{"".join(body)}
<script>document.querySelectorAll("img.swap").forEach(i=>{{const a=i.src,b=i.dataset.alt;
i.onmouseenter=()=>i.src=b;i.onmouseleave=()=>i.src=a;}});</script></body></html>'''
    with open(os.path.join(out_dir, "report.html"), "w", encoding="utf-8") as f:
        f.write(doc)


def make_reference(cur_dir, ref_dir, scenes):
    curs = group(cur_dir)
    if not curs:
        print(f"no <scene>_<NN>.png captures in {cur_dir}")
        return 2
    os.makedirs(ref_dir, exist_ok=True)
    meta_p = os.path.join(ref_dir, "references.json")
    meta = json.load(open(meta_p)) if os.path.isfile(meta_p) else {}
    for scene, files in curs.items():
        if scenes and scene not in scenes:
            continue
        for old in group(ref_dir).get(scene, []):
            os.remove(old)
        for p in files:
            shutil.copy2(p, os.path.join(ref_dir, os.path.basename(p)))
        meta[scene] = dict(frames=len(files), source=os.path.abspath(cur_dir),
                           made=datetime.datetime.now().isoformat(timespec="seconds"))
        print(f"reference {scene}: {len(files)} frame(s)")
    with open(meta_p, "w") as f:
        json.dump(meta, f, indent=1)
    return 0


# ---------------------------------------------------------------- self test
def selftest(out_root):
    rng = np.random.default_rng(1)
    d = os.path.join(out_root, "selftest")
    ref_d, cur_d = os.path.join(d, "ref"), os.path.join(d, "cur")
    for p in (ref_d, cur_d):
        shutil.rmtree(p, ignore_errors=True)
        os.makedirs(p)
    h, w = 540, 960
    yy, xx = np.mgrid[0:h, 0:w]
    base = np.stack([(xx * 255 / w), (yy * 255 / h), 128 + 60 * np.sin(xx / 13.0) * np.cos(yy / 17.0)], -1)
    base += rng.normal(0, 12, (h, w, 3)).astype(np.float32) * (((xx // 32 + yy // 32) % 2) == 0)[..., None]
    base[300:420, 100:400] *= 0.45  # a "shadow"
    base = np.clip(base, 0, 255)

    def save(a, path):
        Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).save(path)

    cases = {}
    for i in range(3):
        save(base, os.path.join(ref_d, f"scene_ok_{i:02d}.png"))
        save(base + rng.normal(0, 2.0, base.shape), os.path.join(cur_d, f"scene_ok_{i:02d}.png"))  # tiny noise only
    cases["scene_ok"] = "PASS"
    for name, fn in {
        "scene_shadow": lambda a: (a.__setitem__((slice(300, 420), slice(100, 400)), a[300:420, 100:400] / 0.45), a)[1],
        "scene_colour": lambda a: (a.__setitem__((slice(60, 200), slice(600, 900)), a[60:200, 600:900][..., ::-1]), a)[1],
        "scene_texture": lambda a: (a.__setitem__((slice(100, 220), slice(120, 330)), np.flipud(np.fliplr(a[100:220, 120:330]))), a)[1],
        "scene_flat": lambda a: (a.__setitem__((slice(0, 120), slice(0, 240)), a[0:120, 0:240].mean((0, 1))), a)[1],
    }.items():
        save(base, os.path.join(ref_d, f"{name}_00.png"))
        save(fn(base.copy()) + rng.normal(0, 1.5, base.shape), os.path.join(cur_d, f"{name}_00.png"))
        cases[name] = "FAIL"
    res = compare_dirs(cur_d, ref_d, os.path.join(d, "report"), {})
    ok = True
    for r in res:
        exp = cases[r["scene"]]
        good = r["status"] == exp or (exp == "PASS" and r["status"] == "WARN")
        ok &= good
        reasons = sorted({x for f in r.get("flags", []) for x in f["reasons"]})
        print(f'{"ok " if good else "BAD"} {r["scene"]:15s} {r["status"]:5s} ssim={r.get("ssim", 0):.4f} bad={r.get("bad_tiles")} {reasons}')
    print("report:", os.path.join(d, "report", "report.html"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compare")
    c.add_argument("--cur", required=True)
    c.add_argument("--ref", required=True)
    c.add_argument("--out")
    c.add_argument("--scenes", default="", help="only these scenes (comma separated)")
    for k, v in DEFAULTS.items():
        c.add_argument("--" + k.replace("_", "-"), type=type(v), default=None)
    m = sub.add_parser("make-reference")
    m.add_argument("--cur", required=True)
    m.add_argument("--ref", required=True)
    m.add_argument("--scenes", default="")
    s = sub.add_parser("selftest")
    s.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "out"))
    a = ap.parse_args()
    if a.cmd == "selftest":
        return selftest(a.out)
    if a.cmd == "make-reference":
        return make_reference(a.cur, a.ref, [x for x in a.scenes.split(",") if x])
    out = a.out or os.path.join(a.cur, "report")
    ov = {k: getattr(a, k) for k in DEFAULTS}
    res = compare_dirs(a.cur, a.ref, out, ov, [x for x in a.scenes.split(",") if x])
    for r in res:
        print(f'{r["status"]:7s} {r["scene"]:24s} ssim={r.get("ssim", 0):.4f} bad_tiles={r.get("bad_tiles", "-")} {r.get("note", "")}')
    print("report:", os.path.abspath(os.path.join(out, "report.html")))
    if any(r["status"] == "MISSING" for r in res):
        return 2
    return 1 if any(r["status"] == "FAIL" for r in res) else 0


if __name__ == "__main__":
    sys.exit(main())
