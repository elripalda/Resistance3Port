#!/usr/bin/env python3
"""Lazy register write-back for the lifted PPU code (post-pass after
regcache.py v141, ppu_fastmem.py v163 and the port hooks).

regcache v141 keeps each guest register in a local (R_n) but writes every
update through to ppu_context as well:  ctx->gpr[n] = R_n = expr;
That is one memory store per lifted instruction, and it keeps every
intermediate CR / XER value alive. Only code that looks at ctx needs ctx to
be current, and within a function that is exactly: a statement that hands
ctx to something else (calls, syscalls, HLE, indirect calls, trampolines,
DRAIN_TRAMPOLINE, hooks), a statement that reads or addresses a cached ctx
field directly, and leaving the function (return / the closing brace).

This pass removes the write-through on every other line and, in front of
each of those points, stores the registers that may differ from ctx there
("dirty" = written on some path since the last point where ctx was made
current). The dirty sets come from a forward may-dataflow over the
function's lines (fallthrough to the next line is always assumed, plus every
goto edge), so an over-approximation only costs a redundant store of the
current value -- never a wrong value: R_n always holds the guest register.
Lines that hand ctx off keep their write-throughs as they are.

A function is left as it is unless every body line is a complete, balanced
statement or a label, every goto target is a label of the function, and the
function was produced by regcache (it has its _RCX_RL macro).

  regcache_lazy.py [--lazy-wb] [--fpr-cache [--lfs-barrier]] in.cpp out.cpp

--fpr-cache also keeps ctx->fpr[n] in double F_n locals (stored/reloaded at
the same points). See scratch\recomp_codegen.md for the measurements: neither
option is a measurable speedup, so the shipped output does not use them.
"""
import re, sys

TAG = '/* lazywb v1 */'
FUNC_RE = re.compile(r'^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{$')
LABEL_RE = re.compile(r'^\s*(loc_[0-9A-F]+):\s*$')
GOTO_RE = re.compile(r'\bgoto\s+(loc_[0-9A-F]+)\s*;')
DECL_RE = re.compile(r'^\s*uint(32|64)_t (R_(\d+|cr|lr|ctr|xer)) = ctx->(gpr\[(\d+)\]|cr|lr|ctr|xer);$')
FIELD = r'(gpr\[(\d+)\]|cr|lr|ctr|xer)'
ANY_FIELD_RE = re.compile(r'\bctx->' + FIELD + r'(?![\w\[])')
BARE_RE = re.compile(r'\bctx\b(?!->)')
CTX_RE = re.compile(r'\bctx->(\w+)')
# fields that are never cached in locals: reading them needs no flush
SAFE_FIELDS = {'fpr', 'vr', 'vscr', 'fpscr', 'reserve_addr', 'reserve_value'}
ASSIGN_OP = r'(?:=(?!=)|\+=|-=|\*=|/=|%=|\|=|&=|\^=|<<=|>>=)'


FPR_RE = re.compile(r'\bctx->fpr\[(\d+)\]')
FPR_ODD_RE = re.compile(r'\bctx->fpr\b(?!\[\d+\])')


def lname(f):
    m = re.match(r'(gpr|fpr)\[(\d+)\]', f)
    if m: return ('R_%s' if m.group(1) == 'gpr' else 'F_%s') % m.group(2)
    return 'R_' + f


def wt_regex(field):
    """ctx->FIELD = followed by an assignment to its local (write-through)"""
    ln = re.escape(lname(field))
    return re.compile(r'\bctx->' + re.escape(field) + r' = (?=(?:(?:--|\+\+)' + ln + r'\b)|(?:' + ln +
                      r'\s*' + ASSIGN_OP + r'))')


def strip_comments(t):
    return re.sub(r'/\*.*?\*/', '', t).strip()


def balanced(t):
    d = 0
    for ch in t:
        if ch in '({[': d += 1
        elif ch in ')}]':
            d -= 1
            if d < 0: return False
    return d == 0


MUTATE = 0   # test-only: deliberately broken variants to prove the differential tester sees errors
FPR_CACHE = False
LFS_BARRIER = False  # --lfs-barrier: force cvtss2sd after lfs (quiets SNaN); off = matches the old build better


def xform(lines, fields, ndecl=0, fprs=()):
    """lines: body lines (without the header / closing brace); the first ndecl
    are the regcache declarations. fprs: FPR numbers to cache in F_n locals
    (empty = FPRs stay in ctx).
    returns (new_lines, why)"""
    wts = {f: wt_regex(f) for f in fields}
    ffields = ['fpr[%d]' % k for k in fprs]
    allf = list(fields) + ffields
    reload_f = ' '.join('F_%d = ctx->fpr[%d];' % (k, k) for k in fprs) if MUTATE != 4 else ''
    n = len(lines)
    labels = {}
    kind = []      # 'label' | 'flush' | 'plain'
    writes = []    # fields written (stripped) on plain lines
    newtext = []
    for i, l in enumerate(lines):
        t = l.strip()
        if i < ndecl:
            kind.append('decl'); writes.append(()); newtext.append(l); continue
        m = LABEL_RE.match(l)
        if m:
            labels[m.group(1)] = i
            kind.append('label'); writes.append(()); newtext.append(l); continue
        c = strip_comments(t)
        if c == '':
            kind.append('plain'); writes.append(()); newtext.append(l); continue
        if '"' in c or "'" in c or c.startswith('#') or c.startswith('else') or c.startswith('case ') \
                or c.startswith('default'):
            return None, 'odd line'
        if not balanced(c) or not (c.endswith(';') or c.endswith('}')):
            return None, 'incomplete statement'
        # flush points
        flush = bool(BARE_RE.search(c)) or (MUTATE != 3 and bool(re.search(r'\breturn\b', c)))
        if not flush:
            for m in CTX_RE.finditer(c):
                if m.group(1) not in SAFE_FIELDS and m.group(1) != 'gpr' and m.group(1) not in fields \
                        and m.group(1) not in ('cr', 'lr', 'ctr', 'xer'):
                    flush = True; break
        if not flush:
            s = l; w = []
            for f, rx in wts.items():
                s2, k = rx.subn('', s)
                if k: w.append(f); s = s2
            if ANY_FIELD_RE.search(strip_comments(s.strip())) or re.search(r'\bctx->gpr\b', strip_comments(s)):
                flush = True     # a read / address of a cached field
            elif fprs and FPR_ODD_RE.search(strip_comments(s)):
                flush = True     # ctx->fpr used other than as ctx->fpr[n]
            else:
                if fprs:
                    used = set(int(k) for k in FPR_RE.findall(s))
                    s = FPR_RE.sub(lambda mm: 'F_' + mm.group(1), s)
                    # lfs: keep the float->double conversion opaque. Otherwise clang
                    # folds a later (float)F_n back to ftmp and a signalling NaN is
                    # stored unquieted, where the ctx round trip ran cvtss2sd.
                    if LFS_BARRIER:
                        s = re.sub(r'\b(F_\d+) = ftmp;', r'\1 = ftmp; __asm__("" : "+x"(\1));', s)
                    for k in sorted(used):
                        if re.search(r'\bF_%d\s*%s' % (k, ASSIGN_OP), s) or re.search(r'&\s*F_%d\b' % k, s) \
                                or re.search(r'(--|\+\+)\s*F_%d\b|\bF_%d\s*(--|\+\+)' % (k, k), s):
                            w.append('fpr[%d]' % k)
                kind.append('plain'); writes.append(tuple(w)); newtext.append(s); continue
        if fprs and re.search(r'\bgoto\b', c):
            return None, 'fpr: goto on a flush line'
        kind.append('flush'); writes.append(()); newtext.append(l)
    succ = [[] for _ in range(n)]
    for i, l in enumerate(lines):
        if i + 1 < n: succ[i].append(i + 1)
        for m in GOTO_RE.finditer(l):
            if m.group(1) not in labels: return None, 'goto outside'
            succ[i].append(labels[m.group(1)])
        if re.search(r'\bgoto\b', l) and not GOTO_RE.search(l): return None, 'odd goto'
    # forward may-dirty dataflow (bitmask over fields)
    fidx = {f: k for k, f in enumerate(allf)}
    gen = [0] * n
    for i in range(n):
        for f in writes[i]: gen[i] |= 1 << fidx[f]
    din = [0] * n
    dout = [-1] * n
    work = list(range(n - 1, -1, -1))
    inq = [True] * n
    while work:
        i = work.pop()
        inq[i] = False
        o = 0 if kind[i] in ('flush', 'decl') else (din[i] | gen[i])
        if o != dout[i]:
            dout[i] = o
            for s in succ[i]:
                nd = din[s] | o
                if nd != din[s]:
                    din[s] = nd
                    if not inq[s]: inq[s] = True; work.append(s)
    def stores(mask):
        return ' '.join('ctx->%s = %s;' % (f, lname(f)) for f in allf if mask >> fidx[f] & 1
                        and not (MUTATE == 2 and f == 'cr') and not (MUTATE == 5 and f.startswith('fpr')))
    out = []
    nflush = 0
    for i in range(n):
        l = newtext[i]
        if kind[i] == 'flush':
            if din[i]:
                ind = l[:len(l) - len(l.lstrip())]
                l = ind + stores(din[i]) + ' ' + l.lstrip()
                nflush += 1
            if reload_f:
                l = l + ' ' + reload_f     # the statement may have changed ctx->fpr
        if kind[i] == 'decl' and i == ndecl - 1 and fprs:
            l = l + '\n' + '\n'.join('        double F_%d = ctx->fpr[%d];' % (k, k) for k in fprs)
        out.append(l)
    end_mask = dout[n - 1] if n else 0
    if end_mask > 0 and MUTATE != 1:
        out.append('        ' + stores(end_mask))
    return out, 'ok'


def process(src):
    lines = src.split('\n')
    out = []; stats = {}; i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i])
        k = i - 1
        while k > 0 and lines[k].startswith('extern \"C\" '): k -= 1   # hook declarations
        if not m or not (k >= 0 and lines[k].startswith('#define _RCX_RL ')):
            out.append(lines[i]); i += 1; continue
        j = i + 1
        while j < len(lines) and lines[j] != '}': j += 1
        body = lines[i + 1:j]
        fields = []
        for l in body:
            d = DECL_RE.match(l)
            if not d: break
            fields.append(d.group(4))
        if not fields:
            why = 'no decls'; nb = None
        else:
            fprs = sorted(set(int(k) for l in body for k in FPR_RE.findall(l))) if FPR_CACHE else []
            nb, why = xform(body, fields, len(fields), fprs)
            if nb is None and fprs:
                nb, why2 = xform(body, fields, len(fields), [])
                why = why + ' -> gpr only ' + why2
            elif fprs:
                why = 'ok+fpr'
        stats[why] = stats.get(why, 0) + 1
        out.append(lines[i])
        out.extend(nb if nb is not None else body)
        if j < len(lines): out.append(lines[j])
        i = j + 1
    return '\n'.join(out), stats


def main(argv):
    global MUTATE, FPR_CACHE, LFS_BARRIER
    opts = [a for a in argv if a.startswith('--')]
    for o in opts:
        if o.startswith('--mutate='): MUTATE = int(o[9:])
    FPR_CACHE = '--fpr-cache' in opts
    LFS_BARRIER = '--lfs-barrier' in opts
    args = [a for a in argv if not a.startswith('--')]
    src = open(args[0], encoding='utf-8', errors='surrogateescape').read()
    if TAG in src or '--lazy-wb' not in opts:
        if TAG in src: print(args[0], 'already processed')
        open(args[1], 'w', encoding='utf-8', errors='surrogateescape', newline='').write(src); return
    res, stats = process(src)
    res = res.replace('#include "ppu_recomp.h"', '#include "ppu_recomp.h"\n' + TAG + (' /* fprcache v2 */' if FPR_CACHE else ''), 1)
    open(args[1], 'w', encoding='utf-8', errors='surrogateescape', newline='').write(res)
    print(args[0], stats)


if __name__ == '__main__':
    main(sys.argv[1:])
