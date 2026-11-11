"""Builds the installer's wizard images from the port's own artwork.

    python tools/installer/make_wizard_images.py

reads   project/launcher_side.png   the 480x1000 sidebar picture (tools/art/make_r*_art.py)
        project/launcher.ico        the launcher icon
        tools/installer/port.iss    PortName / PortShort (for the lettering and the accent colour)
writes  tools/installer/art/wizimage_<w>x<h>.png  WizardImageFile, the Welcome / Finished pages' left panel
        tools/installer/art/wizsmall_<n>.png      WizardSmallImageFile, the inner pages' top-right corner

The sizes are Inno Setup 6.6+'s image areas at 100-250 % DPI (164:314 and square);
Setup picks the closest one. Same script in all three ports; the output is reproducible.
"""
import os
import re

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
OUT = os.path.join(HERE, 'art')

WIZ_SIZES = ((202, 386), (269, 515), (336, 643), (403, 772), (430, 824), (498, 953), (534, 1022))
SMALL_SIZES = (58, 77, 97, 116, 124, 143, 159)
ACCENT = {'ResistancePort': (238, 150, 72), 'Resistance2Port': (232, 76, 40), 'Resistance3Port': (92, 214, 226)}


def font(size, style='Bold Condensed'):
    f = ImageFont.truetype(os.path.join(os.environ.get('WINDIR', r'C:\Windows'), 'Fonts', 'bahnschrift.ttf'), size)
    try:
        f.set_variation_by_name(style)
    except Exception:
        pass
    return f


def iss_define(text, name):
    m = re.search(r'#define\s+%s\s+"([^"]+)"' % name, text)
    return m.group(1) if m else ''


def spaced(draw, xy, text, fnt, spacing):
    """Width of (or, with xy, draws) text with extra letter spacing; returns the width."""
    x = xy[0] if xy else 0
    for ch in text:
        if xy:
            draw.text((x, xy[1]), ch, font=fnt, fill=255)
        x += draw.textlength(ch, font=fnt) + spacing
    return x - spacing - (xy[0] if xy else 0)


def letter(img, lines, accent):
    """Game name in quiet metal lettering near the bottom (the art is dark there)."""
    w, h = img.size
    mask = Image.new('L', img.size, 0)
    d = ImageDraw.Draw(mask)
    y = h - 34
    rows = []
    for text, size, sp, alpha in reversed(lines):
        f = font(size, 'Bold Condensed' if size > 30 else 'SemiBold')
        bb = d.textbbox((0, 0), text, font=f)
        y -= bb[3]
        rows.append((text, f, sp, alpha, y))
        y -= int(size * 0.28)
    rule_y = y + int(lines[0][1] * 0.1)
    # darken the bottom behind the name so it reads over any art
    scrim = Image.new('L', img.size, 0)
    sd = ImageDraw.Draw(scrim)
    top = rule_y - int(h * 0.08)
    for yy in range(max(0, top), h):
        t = min(1.0, (yy - top) / max(1, (h - top) * 0.6))
        sd.line((0, yy, w, yy), fill=int(200 * t * t * (3 - 2 * t)))
    img = Image.alpha_composite(img, Image.merge('RGBA', (*Image.new('RGB', img.size, (8, 8, 10)).split(), scrim)))
    for text, f, sp, alpha, ty in rows:
        tw = spaced(d, None, text, f, sp)
        layer = Image.new('L', img.size, 0)
        spaced(ImageDraw.Draw(layer), ((w - tw) / 2, ty), text, f, sp)
        mask = ImageChops.lighter(mask, layer.point(lambda v, a=alpha: v * a // 255))
    shadow = Image.new('RGBA', img.size, (0, 0, 0, 0))
    shadow.putalpha(ImageChops.offset(mask, 0, 3).filter(ImageFilter.GaussianBlur(4)).point(lambda v: v * 180 // 255))
    img = Image.alpha_composite(img, shadow)
    metal = Image.new('RGBA', img.size)
    md = ImageDraw.Draw(metal)
    for yy in range(h):
        t = max(0.0, min(1.0, (yy - rule_y) / max(1, h - 34 - rule_y)))
        md.line((0, yy, w, yy), fill=(int(228 - 70 * t), int(226 - 72 * t), int(222 - 74 * t), 255))
    metal.putalpha(mask)
    img = Image.alpha_composite(img, metal)
    rim = ImageChops.subtract(mask, ImageChops.offset(mask, 0, 2))
    rl = Image.new('RGBA', img.size, accent + (0,))
    rl.putalpha(rim.point(lambda v: v * 200 // 255))
    img = Image.alpha_composite(img, rl)
    # a thin accent rule above the name, fading out at both ends
    rule = Image.new('L', img.size, 0)
    rd = ImageDraw.Draw(rule)
    for x in range(w):
        t = 1 - abs(x - w / 2) / (w * 0.32)
        if t > 0:
            rd.point((x, rule_y), fill=int(220 * min(1.0, t * 2)))
    return Image.alpha_composite(img, Image.merge('RGBA', (*Image.new('RGB', img.size, accent).split(), rule)))


def main():
    iss = open(os.path.join(HERE, 'port.iss'), encoding='utf-8-sig').read()
    name = re.sub(r'\s*PC Port$', '', iss_define(iss, 'PortName')).replace(':', '')
    accent = ACCENT.get(iss_define(iss, 'PortShort'), (200, 200, 200))
    first, _, rest = name.partition(' ')
    side = Image.open(os.path.join(REPO, 'project', 'launcher_side.png')).convert('RGBA')

    # crop the full width to 164:314 from the top: keeps the tower / spire and the sky above it
    sw = side.width
    ch = round(sw * 314 / 164)
    base = side.crop((0, 0, sw, ch))
    os.makedirs(OUT, exist_ok=True)
    for f in os.listdir(OUT):
        if f.startswith(('wizimage_', 'wizsmall_')):
            os.remove(os.path.join(OUT, f))
    # letter at the largest size once, then scale down, so every size matches
    big = base.resize(WIZ_SIZES[-1], Image.LANCZOS)
    k = big.width / 480
    lines = [(first.upper(), int(64 * k), int(6 * k), 255)]
    if rest:
        lines.append((rest.upper(), int((64 if len(rest) <= 2 else 34) * k), int(5 * k), 235))
    lines.append(('PC PORT', int(20 * k), int(9 * k), 170))
    big = letter(big, lines, accent).convert('RGB')
    for w, h in WIZ_SIZES:
        p = os.path.join(OUT, 'wizimage_%dx%d.png' % (w, h))
        big.resize((w, h), Image.LANCZOS).save(p, optimize=True)
        print('wrote', os.path.relpath(p, REPO), os.path.getsize(p))

    ico = Image.open(os.path.join(REPO, 'project', 'launcher.ico'))
    ico.size = max(ico.info.get('sizes', {ico.size}))
    ico = ico.convert('RGBA')
    for n in SMALL_SIZES:
        p = os.path.join(OUT, 'wizsmall_%d.png' % n)
        ico.resize((n, n), Image.LANCZOS).save(p, optimize=True)
        print('wrote', os.path.relpath(p, REPO), os.path.getsize(p))


if __name__ == '__main__':
    main()
