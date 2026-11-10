"""Draws the Resistance 3 port's icons and launcher art (original art, no game assets).

    python tools/art/make_r3_art.py [--preview DIR]

writes
    project/game.ico           the game's exe icon (Resistance3.exe, project/app.rc)
    project/launcher.ico       the launcher's icon (the same mark with a settings badge)
    project/launcher_side.png  the launcher sidebar picture (embedded by project/launcher.rc)

The mark: a cold steel plate, a tall alien spire on the horizon throwing a pale
cyan beam into the clouds, and "R3" in brushed metal. The sidebar is a ruined
small-town skyline at dusk under the same spire, with fog and falling ash.
Everything is drawn with PIL from shapes and noise (fixed seed, so the output
is reproducible); the only outside input is Windows' Bahnschrift font.
"""
import os
import random
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.normpath(os.path.join(HERE, '..', '..', 'project'))
S = 1024

# palette (shared with project/launcher.cpp)
SLATE = (14, 17, 21)        # sidebar
STEEL_TOP = (46, 54, 62)
STEEL_BOT = (16, 19, 24)
CYAN = (92, 214, 226)       # accent: the spire's light
CYAN_HOT = (220, 252, 255)
RUST = (178, 92, 46)        # secondary accent: embers, the lettering's rim


def font(size, style='Bold Condensed'):
    f = ImageFont.truetype(os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'bahnschrift.ttf'), size)
    try:
        f.set_variation_by_name(style)
    except Exception:
        pass
    return f


def vgrad(w, h, top, bot, a=255):
    im = Image.new('RGBA', (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line((0, y, w, y), fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (a,))
    return im


def rounded_mask(size, radius):
    m = Image.new('L', (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size - 1, size - 1), radius, fill=255)
    return m


def glow(img, mask, layers):
    for blur, gain, col in layers:
        g = mask.filter(ImageFilter.GaussianBlur(blur)).point(lambda v, k=gain: min(255, int(v * k)))
        lay = Image.new('RGBA', img.size, col + (0,))
        lay.putalpha(g)
        img = Image.alpha_composite(img, lay)
    return img


def spire_mask(w, h, cx, base_y, height, width):
    """A tall, tapering alien tower: a needle on a stepped, flared base."""
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    top = base_y - height
    d.polygon([(cx - width * 0.08, base_y), (cx - width * 0.035, top + height * 0.12), (cx, top),
               (cx + width * 0.035, top + height * 0.12), (cx + width * 0.08, base_y)], fill=255)
    d.polygon([(cx - width * 0.5, base_y), (cx - width * 0.16, base_y - height * 0.16),
               (cx + width * 0.16, base_y - height * 0.16), (cx + width * 0.5, base_y)], fill=255)
    for k in (0.32, 0.5, 0.66):                       # ribs on the needle
        y = base_y - height * k
        half = width * (0.08 - 0.045 * k) + width * 0.05
        d.polygon([(cx - half, y), (cx, y - height * 0.035), (cx + half, y), (cx, y + height * 0.012)], fill=255)
    return m


def beam_mask(w, h, cx, top_y, spread):
    m = Image.new('L', (w, h), 0)
    ImageDraw.Draw(m).polygon([(cx - 3, top_y), (cx + 3, top_y), (cx + spread, 0), (cx - spread, 0)], fill=170)
    return m


def draw_mark(small, badge=False):
    img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    radius = 200 if not small else 150
    plate = vgrad(S, S, STEEL_TOP, STEEL_BOT)
    # brushed-metal streaks
    rnd = random.Random(3)
    pd = ImageDraw.Draw(plate)
    for _ in range(260):
        y = rnd.randrange(S)
        c = rnd.randrange(-9, 10)
        base = plate.getpixel((S // 2, y))
        pd.line((0, y, S, y), fill=(max(0, base[0] + c), max(0, base[1] + c), max(0, base[2] + c), 255))
    img.paste(plate, (0, 0), rounded_mask(S, radius))

    horizon = 700 if not small else 820
    cx = S / 2
    sp_h = 560 if not small else 700
    sp_w = 300 if not small else 380
    # sky glow behind the spire
    halo = Image.new('L', (S, S), 0)
    ImageDraw.Draw(halo).ellipse((cx - 330, horizon - sp_h - 120, cx + 330, horizon - sp_h + 260), fill=120)
    img = glow(img, halo, ((120, 1.0, (40, 110, 130)),))
    img = glow(img, beam_mask(S, S, cx, horizon - sp_h + 10, 120 if not small else 150),
               ((30, 0.9, CYAN), (4, 0.8, CYAN_HOT)))
    sm = spire_mask(S, S, cx, horizon, sp_h, sp_w)
    img = glow(img, sm, ((40, 0.9, (30, 120, 140)),))
    dark = Image.new('RGBA', (S, S), (8, 10, 13, 0))
    dark.putalpha(sm)
    img = Image.alpha_composite(img, dark)
    # the needle's lit edge and core lights
    edge = ImageChops.subtract(sm, ImageChops.offset(sm, 7, 0))
    img = glow(img, edge, ((2, 1.0, CYAN),))
    core = Image.new('L', (S, S), 0)
    cd = ImageDraw.Draw(core)
    for k in (0.32, 0.5, 0.66, 0.9):
        y = horizon - sp_h * k
        cd.ellipse((cx - 9, y - 9, cx + 9, y + 9), fill=255)
    img = glow(img, core, ((22, 1.2, CYAN), (3, 1.0, CYAN_HOT)))
    # ground line
    gd = ImageDraw.Draw(img)
    gd.rectangle((0, horizon, S, S), fill=(10, 12, 15, 255))
    gd.line((0, horizon, S, horizon), fill=(70, 120, 130, 255), width=4)

    if True:   # "R3"; the 16-32 px sizes only fit a big "3"
        label, f, base_y = ('R3', font(500), 930) if not small else ('3', font(860), 990)
        txt = Image.new('L', (S, S), 0)
        td = ImageDraw.Draw(txt)
        bb = td.textbbox((0, 0), label, font=f)
        w, h = bb[2] - bb[0], bb[3] - bb[1]
        x, y = (S - w) / 2 - bb[0], base_y - h - bb[1]
        td.text((x, y), label, font=f, fill=255)
        metal = Image.new('RGBA', (S, S))
        md = ImageDraw.Draw(metal)
        for yy in range(S):
            t = max(0.0, min(1.0, (yy - (base_y - h)) / h))
            md.line((0, yy, S, yy), fill=(int(226 - 100 * t), int(232 - 98 * t), int(236 - 92 * t), 255))
        shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
        shadow.putalpha(ImageChops.offset(txt, 0, 16).filter(ImageFilter.GaussianBlur(12)).point(lambda v: v * 210 // 255))
        img = Image.alpha_composite(img, shadow)
        metal.putalpha(txt)
        img = Image.alpha_composite(img, metal)
        rim = ImageChops.subtract(txt, ImageChops.offset(txt, 0, 10))
        rl = Image.new('RGBA', (S, S), CYAN + (0,))
        rl.putalpha(rim.point(lambda v: v * 190 // 255))
        img = Image.alpha_composite(img, rl)
        low = ImageChops.subtract(txt, ImageChops.offset(txt, 0, -8))
        ll = Image.new('RGBA', (S, S), RUST + (0,))
        ll.putalpha(low.point(lambda v: v * 150 // 255))
        img = Image.alpha_composite(img, ll)

    if badge:   # the launcher: a small settings cog in the corner
        bdg = Image.new('L', (S, S), 0)
        bd = ImageDraw.Draw(bdg)
        bx, by, r = (S - 190, 190, 120) if not small else (S - 200, 200, 150)
        for i in range(8):
            import math
            a = i * math.pi / 4
            px, py = bx + math.cos(a) * r * 0.92, by + math.sin(a) * r * 0.92
            bd.ellipse((px - r * 0.24, py - r * 0.24, px + r * 0.24, py + r * 0.24), fill=255)
        bd.ellipse((bx - r * 0.82, by - r * 0.82, bx + r * 0.82, by + r * 0.82), fill=255)
        bd.ellipse((bx - r * 0.34, by - r * 0.34, bx + r * 0.34, by + r * 0.34), fill=0)
        sh = Image.new('RGBA', (S, S), (0, 0, 0, 0))
        sh.putalpha(bdg.filter(ImageFilter.GaussianBlur(14)).point(lambda v: v * 200 // 255))
        img = Image.alpha_composite(img, sh)
        cog = Image.new('RGBA', (S, S), CYAN + (0,))
        cog.putalpha(bdg)
        img = Image.alpha_composite(img, cog)

    edge = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle((6, 6, S - 7, S - 7), radius - 6, outline=(255, 255, 255, 36), width=10)
    img = Image.alpha_composite(img, edge)
    img.putalpha(ImageChops.multiply(img.getchannel('A'), rounded_mask(S, radius)))
    return img


def save_ico(path, big, small):
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    frames = [(small if s <= 32 else big).resize((s, s), Image.LANCZOS) for s in sizes]
    frames[-1].save(path, format='ICO', sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    print('wrote', path)


def draw_sidebar(w=480, h=1000):
    """The launcher sidebar's picture (shown below the title, faded in from the
    sidebar colour by the launcher): dusk over a ruined town, the spire on the
    horizon."""
    rnd = random.Random(1951)
    img = vgrad(w, h, (20, 28, 36), (58, 66, 70))
    horizon = int(h * 0.70)
    # clouds: soft noise bands, lit cyan around the beam
    clouds = Image.new('L', (w, h), 0)
    cd = ImageDraw.Draw(clouds)
    for _ in range(140):
        cx, cy = rnd.uniform(-60, w + 60), rnd.uniform(0, horizon * 0.8)
        rx, ry = rnd.uniform(40, 150), rnd.uniform(8, 26)
        cd.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=rnd.randrange(20, 70))
    clouds = clouds.filter(ImageFilter.GaussianBlur(14))
    cl = Image.new('RGBA', (w, h), (120, 134, 142, 0))
    cl.putalpha(clouds)
    img = Image.alpha_composite(img, cl)
    sx = int(w * 0.64)
    sp_h = int(h * 0.52)
    halo = Image.new('L', (w, h), 0)
    ImageDraw.Draw(halo).ellipse((sx - 160, horizon - sp_h - 120, sx + 160, horizon - sp_h + 140), fill=110)
    img = glow(img, halo, ((70, 1.0, (50, 120, 136)),))
    img = glow(img, beam_mask(w, h, sx, horizon - sp_h + 4, 46), ((14, 0.8, CYAN), (2, 0.7, CYAN_HOT)))
    sm = spire_mask(w, h, sx, horizon, sp_h, 130)
    far = Image.new('RGBA', (w, h), (26, 34, 40, 0))
    far.putalpha(sm)
    img = Image.alpha_composite(img, far)
    lights = Image.new('L', (w, h), 0)
    ld = ImageDraw.Draw(lights)
    for k in (0.32, 0.5, 0.66, 0.9):
        y = horizon - sp_h * k
        ld.ellipse((sx - 3, y - 3, sx + 3, y + 3), fill=255)
    img = glow(img, lights, ((8, 1.3, CYAN), (1, 1.0, CYAN_HOT)))
    # fog over the horizon
    fog = vgrad(w, h, (0, 0, 0), (0, 0, 0))
    fm = Image.new('L', (w, h), 0)
    fd = ImageDraw.Draw(fm)
    for y in range(h):
        t = 1 - min(1.0, abs(y - horizon) / (h * 0.13))
        fd.line((0, y, w, y), fill=int(120 * t * t))
    fog = Image.new('RGBA', (w, h), (96, 110, 116, 0))
    fog.putalpha(fm)
    img = Image.alpha_composite(img, fog)
    # the town: houses, a church steeple, a water tower, a grain elevator, broken roofs
    town = Image.new('L', (w, h), 0)
    td = ImageDraw.Draw(town)
    x = -10
    while x < w + 10:
        bw = rnd.randrange(26, 64)
        kind = rnd.random()
        base = horizon + rnd.randrange(6, 30)
        if kind < 0.55:                                  # house with a pitched roof (some broken)
            wall = rnd.randrange(22, 40)
            td.rectangle((x, base - wall, x + bw, h), fill=255)
            peak = base - wall - rnd.randrange(12, 26)
            if rnd.random() < 0.35:
                td.polygon([(x, base - wall), (x + bw * 0.4, peak), (x + bw * 0.55, base - wall + 8), (x + bw, base - wall)], fill=255)
            else:
                td.polygon([(x - 3, base - wall), (x + bw / 2, peak), (x + bw + 3, base - wall)], fill=255)
        elif kind < 0.7:                                 # flat-roofed store, ragged top
            top = base - rnd.randrange(40, 70)
            pts = [(x, h), (x, top)]
            for i in range(1, 6):
                pts.append((x + bw * i / 6, top + rnd.randrange(0, 12)))
            pts += [(x + bw, top), (x + bw, h)]
            td.polygon(pts, fill=255)
        elif kind < 0.8:                                 # steeple
            td.rectangle((x + bw * 0.3, base - 70, x + bw * 0.7, h), fill=255)
            td.polygon([(x + bw * 0.25, base - 70), (x + bw * 0.5, base - 130), (x + bw * 0.75, base - 70)], fill=255)
        elif kind < 0.9:                                 # water tower
            td.ellipse((x + 4, base - 120, x + bw - 4, base - 84), fill=255)
            td.rectangle((x + 4, base - 102, x + bw - 4, base - 76), fill=255)
            for lx in (x + 8, x + bw - 10):
                td.line((lx, base - 76, lx + (6 if lx < x + bw / 2 else -6), base), fill=255, width=3)
        else:                                            # grain elevator
            td.rectangle((x, base - 110, x + bw * 0.6, h), fill=255)
            td.rectangle((x + bw * 0.6, base - 60, x + bw, h), fill=255)
        x += bw + rnd.randrange(-4, 10)
    town_l = Image.new('RGBA', (w, h), (9, 11, 14, 0))
    town_l.putalpha(town)
    img = Image.alpha_composite(img, town_l)
    # a few lit windows and embers
    win = Image.new('L', (w, h), 0)
    wd = ImageDraw.Draw(win)
    for _ in range(14):
        wx, wy = rnd.randrange(0, w), rnd.randrange(horizon - 40, horizon + 30)
        if town.getpixel((min(w - 1, wx), min(h - 1, wy))):
            wd.rectangle((wx, wy, wx + 3, wy + 4), fill=200)
    img = glow(img, win, ((6, 1.0, RUST), (0.5, 1.0, (255, 190, 120))))
    # falling ash
    ash = Image.new('L', (w, h), 0)
    ad = ImageDraw.Draw(ash)
    for _ in range(420):
        ax, ay, r = rnd.uniform(0, w), rnd.uniform(0, h), rnd.uniform(0.6, 2.2)
        ad.ellipse((ax - r, ay - r, ax + r, ay + r), fill=rnd.randrange(60, 200))
    ash = ash.filter(ImageFilter.GaussianBlur(0.7))
    al = Image.new('RGBA', (w, h), (210, 220, 226, 0))
    al.putalpha(ash)
    img = Image.alpha_composite(img, al)
    # the ground fades back into the sidebar colour at the bottom
    fade = Image.new('L', (w, h), 0)
    fd = ImageDraw.Draw(fade)
    for y in range(h):
        t = max(0.0, (y - h * 0.80) / (h * 0.20))
        fd.line((0, y, w, y), fill=int(255 * min(1.0, t)))
    bot = Image.new('RGBA', (w, h), SLATE + (0,))
    bot.putalpha(fade)
    img = Image.alpha_composite(img, bot)
    return img.convert('RGB')


def main():
    preview = None
    if '--preview' in sys.argv:
        preview = sys.argv[sys.argv.index('--preview') + 1]
        os.makedirs(preview, exist_ok=True)
    big, small = draw_mark(False), draw_mark(True)
    save_ico(os.path.join(PROJECT, 'game.ico'), big, small)
    bigb, smallb = draw_mark(False, True), draw_mark(True, True)
    save_ico(os.path.join(PROJECT, 'launcher.ico'), bigb, smallb)
    side = draw_sidebar()
    side.save(os.path.join(PROJECT, 'launcher_side.png'), optimize=True)
    print('wrote', os.path.join(PROJECT, 'launcher_side.png'))
    if preview:
        big.resize((512, 512), Image.LANCZOS).save(os.path.join(preview, 'game_icon.png'))
        bigb.resize((512, 512), Image.LANCZOS).save(os.path.join(preview, 'launcher_icon.png'))
        sheet = Image.new('RGBA', (16 + 24 + 32 + 48 + 64 + 128 + 70, 140), (40, 40, 44, 255))
        x = 10
        for s in (16, 24, 32, 48, 64, 128):
            sheet.alpha_composite((smallb if s <= 32 else bigb).resize((s, s), Image.LANCZOS), (x, 6))
            x += s + 10
        sheet.save(os.path.join(preview, 'icon_sizes.png'))


if __name__ == '__main__':
    main()
