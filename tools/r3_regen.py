#!/usr/bin/env python3
"""Resistance 3 port -- regenerate all game-derived code from the player's own
decrypted EBOOT.elf (Resistance 3 is one program: EBOOT.BIN == game.self).

    python tools/r3_regen.py --eboot <EBOOT.elf> [--port <port dir>] [--only ppu|spu]

Writes, inside the port folder:
    recompiled/          PPU code
    project/spu/<mod>/   lifted SPU images: the Wws job manager (policy module)
                         and the embedded SPU ELF programs. Job modules not
                         lifted yet run in the SPU interpreter.
Nothing here is taken from the game except through the lifters run on the
player's own file; tools/hooks/r3.json holds the port's own edits.
"""
import argparse, bisect, glob, hashlib, json, os, re, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from regen_common import run, ppu, fix_includes

# Resistance 3 (BCUS98176 v1.00): the Wws_Job policy module the game passes to
# cellSpursAddWorkload (VA 0xEB9E00, 0x3100 bytes, runs at LS 0xA00).
PM = dict(off=0xEA9E00, size=0x3100, base=0xA00, entry=0xA00)


def fnv1a64(b):
    h = 1469598103934665603
    for x in b: h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def ila_r2(w):
    return (w >> 25) == 0x21 and (w & 0x7F) == 2


def ila_head(d, o):
    return o + 0x30 <= len(d) and all(ila_r2(w) for w in struct.unpack_from('>4I', d, o))


def off2va(d, off):
    phoff, phn = struct.unpack_from('>Q', d, 0x20)[0], struct.unpack_from('>H', d, 0x38)[0]
    for i in range(phn):
        t, _, o, v, _, fs = struct.unpack_from('>IIQQQQ', d, phoff + i * 56)
        if t == 1 and o <= off < o + fs: return v + off - o
    raise SystemExit('file offset 0x%X not in a PT_LOAD segment' % off)


def spu_elfs(d):
    """(file offset, length, entry, [(seg file offset, vaddr, filesz)]) of each embedded SPU ELF program."""
    out = []
    for m in re.finditer(b'\x7fELF\x01\x02\x01', d):
        o = m.start()
        if struct.unpack_from('>H', d, o + 18)[0] != 0x17: continue
        phoff, shoff = struct.unpack_from('>I', d, o + 28)[0], struct.unpack_from('>I', d, o + 32)[0]
        phnum, shnum = struct.unpack_from('>H', d, o + 44)[0], struct.unpack_from('>H', d, o + 48)[0]
        end = max([shoff + shnum * 40] + [struct.unpack_from('>I', d, o + phoff + k * 32 + 4)[0] +
                                         struct.unpack_from('>I', d, o + phoff + k * 32 + 16)[0] for k in range(phnum)])
        segs = []
        for k in range(phnum):
            t, off, va, _, fs = struct.unpack_from('>5I', d, o + phoff + k * 32)
            if t == 1 and fs: segs.append((o + off, va, fs))
        out.append((o, end, struct.unpack_from('>I', d, o + 24)[0], segs))
    return out


# Insomniac's igPm workload (maxContention 5): a 0x230-byte policy module (the
# loader) GETs the job kernel from the EBOOT to LS 0x3F580 and calls it; the
# kernel GETs each job's code from its header's EA to its LS destination.
IGPM = dict(off=0xE18F80, size=0x230, base=0xA00, entry=0xA00, img=2)
IGKERNEL = dict(off=0xE18A00, size=0x510, base=0x3F580, entry=0x3F5A0, code_end=0x3F9A8, img=3)
IGJOB_IMG, IGPIC_IMG, WWSPIC_IMG = 400, 600, 900


def spu_blobs(d, elfs):
    """Every SPU code blob in the EBOOT that starts with the 4 x `ila $r2`
    signature, classified:
      c0de  WWS job module (streamed to LS 0x4000 by the job manager)
      dead  WWS plugin, position independent (copied to a heap address)
      igA   Insomniac job: header quad at +0x10 (w0 hi16<<2 = LS dest,
            w1 hi16<<2 = size, w2 lo16<<2 = entry), fixed address
      igB   Insomniac PIC module: halfwords h0..h7 at +0x10 with h6 = h0+1,
            h7 = 0x8000; h1..h3 << 2 = sizes, h5 << 2 = entry offset
    Returns [(kind, file off, span, base, entry)]; a blob ends at the next blob,
    the next embedded ELF or a policy module, whichever comes first."""
    heads = [o for o in range(0, len(d) - 0x30, 16) if ila_head(d, o)]
    fixed = {PM['off'], IGPM['off'], IGKERNEL['off']}
    elf_segs = {s[0] for e in elfs for s in e[3]}
    bounds = sorted(set(heads) | {e[0] for e in elfs} | {s[0] for e in elfs for s in e[3]} |
                    fixed | {PM['off'] + PM['size'], len(d)})
    nb = lambda o: bounds[bisect.bisect_right(bounds, o)]
    out = []
    for o in heads:
        if o in fixed: continue
        lim = nb(o) - o
        w = struct.unpack_from('>8I', d, o + 0x10)
        hw = struct.unpack_from('>8H', d, o + 0x10)
        if w[4] == 0xC0DEC0DE:
            out.append(('c0de', o, min(w[1], lim), None, w[0]))
        elif w[6] == 0xDEADDEAD:
            span = lim
            if span > 0x8000:   # the last plugin runs into texture data: cut at the first zero word
                span = next((k for k in range(0x30, lim, 4) if not struct.unpack_from('>I', d, o + k)[0]), lim)
                span = (span + 15) & ~15
            out.append(('dead', o, span, 0, 0x30))
        elif hw[7] == 0x8000 and hw[6] == hw[0] + 1:
            span = min(max(hw[1], hw[2], hw[3]) << 2, lim)
            e = hw[5] << 2
            if 0x20 <= e < span: out.append(('igB', o, span, 0, e))
        elif o not in elf_segs:
            lsa, size, e = (w[0] >> 16) << 2, (w[1] >> 16) << 2, (w[2] & 0xFFFF) << 2
            if size >= 0x20 and lsa + size <= IGKERNEL['base'] and lsa < e < lsa + size:
                out.append(('igA', o, size, lsa, e))
    return out


FULL = True


def lift_cmd(tk, pe, out, n, base, entry_extra=(), lo=None, hi=None, pic=False, rel=False):
    """spu_lifter.py command for one image, with whole-image entry discovery
    (tools/spu_full_entries.py): every call return, block leader and validated
    gap run becomes an entry, so code reached through run-time pointers is lifted."""
    cmd = [sys.executable, os.path.join(tk, 'spu_lifter.py'), '--auto-functions', pe, '--base', hex(base),
           '--symbol-prefix', n + '_', '-o', os.path.join(out, n), '--source-name', 'spu_recomp.c',
           '--header-name', 'spu_recomp.h', '--return-entries', '--merge-chunks',
           '--report', pe + '.report.json']
    if pic: cmd.append('--pic')
    if hi: cmd += ['--code-end', hex(hi)]
    if entry_extra: cmd += ['--extra-funcs', ','.join(hex(a) for a in sorted(set(entry_extra)))]
    if FULL:
        from spu_full_entries import discover_elf
        r = discover_elf(open(pe, 'rb').read(), base=base, lo=lo, hi=hi, rel=rel, extra=list(entry_extra))
        if r['observed']:
            f = pe + '.observed.txt'; open(f, 'w').write(','.join(hex(a) for a in sorted(r['observed'])))
            cmd += ['--observed-entries', '@' + f]
        if r['gaps']:
            f = pe + '.gaps.txt'; open(f, 'w').write(','.join('%X:%X' % g for g in r['gaps']))
            cmd += ['--gap-funcs', '@' + f]
    return cmd


def code_span(pe, base, size):
    """LS bytes from the image base to the end of the last lifted function:
    the resident span. Data GETs past the code must not evict the lift."""
    try:
        fs = json.load(open(pe + '.report.json'))['functions']
        end = max(e for s, e in fs)
    except Exception:
        return size
    return min(size, (end - base + 15) & ~15) or size


def write_workloads(path, d, plain_elfs, igjobs, igpics, wwspics):
    """Registration TU: each lifted image keyed by the FNV-1a-64 fingerprint the
    runtime computes over the image the guest hands it (spu_workload.c), and
    each streamed code image by its source EA (resident spans)."""
    L = ['/* Generated by tools/r3_regen.py -- do not edit.',
         ' * Registers Resistance 3\'s lifted SPU images by content fingerprint',
         ' * and its streamed SPU code images by source EA. */',
         '#include "spu_context.h"', '#include "spu_workload.h"',
         'extern void spu_begin_image(int image_id);',
         'extern void spu_overlay_register_region(unsigned content_ea, unsigned span, int image_id);',
         'extern void spu_overlay_register_pic(unsigned src_ea, unsigned span, int image_id);',
         'extern void spu_overlay_register_pic_at(unsigned src_ea, unsigned span, int image_id, unsigned min_base);',
         'extern void spurs_pm_block_reads(int on);',
         'extern void pm_wwsjob_spu_func_%08X(spu_context*);' % PM['entry'],
         'extern void pm_wwsjob_spu_recomp_register(void);',
         'extern void igpm_spu_func_%08X(spu_context*);' % IGPM['entry'],
         'extern void igpm_spu_recomp_register(void);',
         'extern void igkernel_spu_recomp_register(void);']
    for i, (o, n, e, _) in plain_elfs:
        L += ['extern void spuelf%d_spu_func_%08X(spu_context*);' % (i, e), 'extern void spuelf%d_spu_recomp_register(void);' % i]
    for n, *_ in igjobs + igpics + wwspics:
        L += ['extern void %s_spu_recomp_register(void);' % n]
    L += ['',
          '/* igPm reads its SPU index from SNR1 and its kernel parks in',
          ' * rdch SPU_RdEventStat: blocking channel reads, as on the SPU. */',
          'static void igpm_entry(spu_context* ctx)', '{',
          '    spurs_pm_block_reads(1);',
          '    igpm_spu_func_%08X(ctx);' % IGPM['entry'], '}', '',
          'void r3_spu_register_all(void)', '{',
          '    spu_begin_image(1); pm_wwsjob_spu_recomp_register();',
          '    spu_workload_register_img(0x%016XULL, pm_wwsjob_spu_func_%08X, 1, "pm_wwsjob");'
          % (fnv1a64(d[PM['off']:PM['off'] + PM['size']]), PM['entry']),
          '    spu_begin_image(%d); igpm_spu_recomp_register();' % IGPM['img'],
          '    spu_workload_register_img(0x%016XULL, igpm_entry, %d, "igpm");'
          % (fnv1a64(d[IGPM['off']:IGPM['off'] + IGPM['size']]), IGPM['img']),
          '    spu_begin_image(%d); igkernel_spu_recomp_register();' % IGKERNEL['img'],
          '    spu_overlay_register_region(0x%08Xu, 0x%Xu, %d);'
          % (off2va(d, IGKERNEL['off']), IGKERNEL['span'], IGKERNEL['img'])]
    for i, (o, n, e, _) in plain_elfs:
        L += ['    spu_begin_image(%d); spuelf%d_spu_recomp_register();' % (300 + i, i),
              '    spu_workload_register_img(0x%016XULL, spuelf%d_spu_func_%08X, %d, "spuelf%d");'
              % (fnv1a64(d[o:o + n]), i, e, 300 + i, i)]
    for n, va, span, img in igjobs:
        L += ['    spu_begin_image(%d); %s_spu_recomp_register(); spu_overlay_register_region(0x%08Xu, 0x%Xu, %d);'
              % (img, n, va, span, img)]
    for n, va, span, img in igpics:
        L += ['    spu_begin_image(%d); %s_spu_recomp_register(); spu_overlay_register_pic_at(0x%08Xu, 0x%Xu, %d, 0u);'
              % (img, n, va, span, img)]
    for n, va, span, img in wwspics:
        L += ['    spu_begin_image(%d); %s_spu_recomp_register(); spu_overlay_register_pic(0x%08Xu, 0x%Xu, %d);'
              % (img, n, va, span, img)]
    L += ['    spu_begin_image(0);', '}', '',
          '__attribute__((constructor)) static void r3_spu_register_all_ctor(void)', '{', '    r3_spu_register_all();', '}', '']
    open(path, 'w', newline='\n').write('\n'.join(L))


def spu(port, tk, eboot, work):
    sys.path.insert(0, tk)
    from wrap_spu_elf import wrap
    from concurrent.futures import ThreadPoolExecutor
    spudir = os.path.join(port, 'project', 'spu')
    d = open(eboot, 'rb').read()
    env = dict(os.environ, PYTHONPATH=tk)
    out = os.path.join(work, 'spu')
    for x in (out, os.path.join(work, 'jobmods'), os.path.join(work, 'img')):
        if os.path.isdir(x): shutil.rmtree(x)
    img_dir = os.path.join(work, 'img'); os.makedirs(img_dir)
    elfs = spu_elfs(d)
    blobs = spu_blobs(d, elfs)
    # ELFs whose segment is a WWS job module are lifted as job modules below.
    c0de_offs = {o for k, o, *_ in blobs if k == 'c0de'}
    plain_elfs = [(i, (o, n, e, segs)) for i, (o, n, e, segs) in enumerate(elfs)
                  if not any(s[0] in c0de_offs for s in segs)]
    print('[spu] %d SPU ELFs (%d programs), blobs: %s' % (len(elfs), len(plain_elfs),
          ', '.join('%s=%d' % (k, sum(1 for b in blobs if b[0] == k)) for k in ('c0de', 'dead', 'igA', 'igB'))), flush=True)
    jobs = []   # (name, cmd, pe, base, size)

    def image(n, raw, base, entry):
        pe = os.path.join(img_dir, n + '.elf')
        open(pe, 'wb').write(wrap(raw, base=base, entry=entry))
        return pe

    lift = [sys.executable, os.path.join(tk, 'spu_lifter.py'), '--auto-functions']
    common = lambda n: ['--symbol-prefix', n + '_', '-o', os.path.join(out, n),
                        '--source-name', 'spu_recomp.c', '--header-name', 'spu_recomp.h']
    # Wws job manager. 0xA2C: its SPU interrupt handler (installed as
    # "br 0xA2C" at LS 0); nothing branches to it statically.
    pe = image('pm_wwsjob', d[PM['off']:PM['off'] + PM['size']], PM['base'], PM['entry'])
    jobs.append(('pm_wwsjob', lift + [pe, '--base', hex(PM['base'])] + common('pm_wwsjob') +
                 ['--return-entries', '--extra-funcs', '0xa2c'], pe, 0, 0))
    # igPm loader (policy module) and the job kernel it streams in
    pe = image('igpm', d[IGPM['off']:IGPM['off'] + IGPM['size']], IGPM['base'], IGPM['entry'])
    jobs.append(('igpm', lift_cmd(tk, pe, out, 'igpm', IGPM['base'], [IGPM['entry']], lo=IGPM['base'] + 0x10), pe, 0, 0))
    k = IGKERNEL
    pe = image('igkernel', d[k['off']:k['off'] + k['size']], k['base'], k['entry'])
    jobs.append(('igkernel', lift_cmd(tk, pe, out, 'igkernel', k['base'], [k['entry']], lo=k['base'] + 0x20,
                                      hi=k['code_end']), pe, k['base'], k['code_end'] - k['base']))
    for i, (o, n, e, _) in plain_elfs:
        nm = 'spuelf%d' % i
        pe = os.path.join(img_dir, nm + '.elf'); open(pe, 'wb').write(d[o:o + n])
        jobs.append((nm, lift + [pe] + common(nm), pe, 0, 0))
    igjobs, igpics, wwspics = [], [], []
    for kind, o, span, base, e in blobs:
        raw = d[o:o + span]; va = off2va(d, o)
        if kind == 'igA':
            nm = 'igjob_%08X' % va
            pe = image(nm, raw, base, e)
            jobs.append((nm, lift_cmd(tk, pe, out, nm, base, [e], lo=base + 0x20), pe, base, span))
            igjobs.append([nm, va, span, IGJOB_IMG + len(igjobs)])
        elif kind == 'igB':
            nm = 'igpic_%08X' % va
            pe = image(nm, raw, 0, e)
            jobs.append((nm, lift_cmd(tk, pe, out, nm, 0, [0x20, e], lo=0x20, pic=True, rel=True), pe, 0, span))
            igpics.append([nm, va, span, IGPIC_IMG + len(igpics)])
        elif kind == 'dead':
            nm = 'wwspic_%08X' % va
            ex = [0x30]
            f0 = struct.unpack_from('>I', d, o + 0x10)[0]
            if 0x30 <= f0 < span and not f0 & 3: ex.append(f0)
            pe = image(nm, raw, 0, 0x30)
            jobs.append((nm, lift_cmd(tk, pe, out, nm, 0, ex, lo=0x30, pic=True, rel=True), pe, 0, span))
            wwspics.append([nm, va, span, WWSPIC_IMG + len(wwspics)])

    def run_one(j):
        nm, cmd, pe, base, size = j
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('lift of %s FAILED:\n%s' % (nm, (r.stdout + r.stderr)[-2000:]))
        return nm
    print('[spu] lifting %d images...' % len(jobs), flush=True)
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        for i, nm in enumerate(ex.map(run_one, jobs)):
            if i % 25 == 0: print('  [%d/%d] %s' % (i + 1, len(jobs), nm), flush=True)
    spans = {nm: code_span(pe, base, size) for nm, _, pe, base, size in jobs if size}
    for lst in (igjobs, igpics, wwspics):
        for x in lst: x[2] = spans[x[0]]
    IGKERNEL['span'] = spans['igkernel']
    mods = [j[0] for j in jobs]
    # WWS job modules (C0DEC0DE), lifted at their link base like R2's
    jm = os.path.join(work, 'jobmods'); os.makedirs(jm)
    for kind, o, span, _, e in blobs:
        if kind != 'c0de': continue
        raw = d[o:o + span]
        open(os.path.join(jm, 'jobmod_%s_e%X.bin' % (hashlib.sha1(raw[:16]).hexdigest()[:12], e)), 'wb').write(raw)
    print('[spu] lifting WWS job modules...', flush=True)
    # 0x5300: reached only through a function pointer (bisl from 0xD974 in the
    # 0xDC50-byte module); no static branch names it, so --full folds it into 0x5200.
    jenv = dict(env, LBP_JOBMOD_EXTRA_FUNCS='0x5300')
    run([sys.executable, os.path.join(HERE, 'lift_jobmods_base.py'), '--jobmods', jm, '--out', out,
         '--register', os.path.join(work, 'jobmods_register.c'), '--full'], env=jenv)
    mods += sorted(os.path.basename(x) for x in glob.glob(os.path.join(out, 'jobmod_*')) if os.path.isdir(x))

    os.makedirs(spudir, exist_ok=True)
    for x in glob.glob(os.path.join(spudir, '*')):
        if os.path.isdir(x): shutil.rmtree(x)
    shutil.copy(os.path.join(work, 'jobmods_register.c'), os.path.join(spudir, 'jobmods_register.c'))
    write_workloads(os.path.join(spudir, 'spu_workloads.c'), d, plain_elfs, igjobs, igpics, wwspics)
    print('[spu] register cache...', flush=True)

    def post(n):
        x = os.path.join(out, n); fix_includes(x)
        f = os.path.join(x, 'spu_recomp.c')
        for cmd in ([os.path.join(HERE, 'spu_regcache.py'), f, f], [os.path.join(HERE, 'spu_directhop.py'), f]):
            subprocess.run([sys.executable] + cmd, check=True, capture_output=True)
        if n.startswith(('jobmod_', 'wwspic_')):
            for t in ('spu_writeback.py', 'spu_group_entry.py', 'spu_loop_ib.py'):
                subprocess.run([sys.executable, os.path.join(HERE, t), f, f], check=True, capture_output=True)
        shutil.copytree(x, os.path.join(spudir, n))
    with ThreadPoolExecutor(os.cpu_count() or 4) as ex:
        list(ex.map(post, mods))
    print('[spu] %d images in %s' % (len(mods), spudir), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--eboot', required=True)
    ap.add_argument('--port', default=os.path.dirname(HERE)); ap.add_argument('--work', default=None)
    ap.add_argument('--only', choices=['ppu', 'spu'], default=None)
    ap.add_argument('--chunks', type=int, default=28)
    a = ap.parse_args()
    port = os.path.abspath(a.port); tk = os.path.join(port, 'ps3recomp', 'tools')
    work = os.path.abspath(a.work or os.path.join(port, 'build_regen'))
    os.makedirs(work, exist_ok=True)
    if a.only in (None, 'ppu'):
        ppu(port, tk, a.eboot, 'recompiled', 'r3', a.chunks, True, os.path.join(HERE, 'hooks', 'r3.json'), work)
    if a.only in (None, 'spu'):
        spu(port, tk, os.path.abspath(a.eboot), work)
    print('done: game code regenerated.')


if __name__ == '__main__':
    main()
