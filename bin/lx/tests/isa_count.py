#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Count the instructions real binaries use, per architecture, from llvm-objdump output.
#   isa_count.py ARCH FILE...      ARCH: amd64 | arm64 | riscv64
# Prints: static instruction count, distinct mnemonics, distinct forms (mnemonic plus
# operand kinds: for x86 this approximates distinct encodings), how many mnemonics cover
# 99% and 99.9% of occurrences, and the integer / scalar FP / vector split.
import collections, re, subprocess, sys

arch, files = sys.argv[1], [a for a in sys.argv[2:] if not a.startswith('--')]

def classify(arch, mn, ops):
    if arch == 'amd64':
        if re.search(r'%[xyz]mm', ops) or mn.startswith('v'):
            # scalar SSE floating point: ...ss / ...sd forms without packed data
            if re.search(r'(ss|sd)$', mn) and not mn.startswith(('movs', 'cmps')) and not re.search(r'%[yz]mm', ops) or mn.startswith(('cvt', 'ucomis', 'comis')):
                return 'fp'
            return 'vector'
        if mn.startswith('f') and not mn.startswith(('fs', 'fe')) or mn in ('fld', 'fstp'):
            return 'fp'
        return 'int'
    if arch == 'arm64':
        if re.search(r'\bv\d+\.|\bq\d+\b|\bz\d+\.', ops):
            return 'vector'
        if mn.startswith(('f', 'scvtf', 'ucvtf')) or re.search(r'\b[dsh]\d+\b', ops):
            return 'fp'
        return 'int'
    if arch == 'riscv64':
        mn = mn[2:] if mn.startswith('c.') else mn
        if mn.startswith('v') and mn not in ('',):
            return 'vector'
        if mn.startswith('f') and not mn.startswith(('fence',)):
            return 'fp'
        return 'int'
    return 'int'

def form(arch, mn, ops):
    if not ops: return mn
    kinds = []
    for o in [x.strip() for x in re.split(r',(?![^(\[]*[)\]])', ops)]:
        if arch == 'amd64':
            if o.startswith('$'): k = 'imm'
            elif o.startswith('%'):
                r = o[1:]
                k = 'xmm' if r.startswith('xmm') else 'ymm' if r.startswith('ymm') else 'zmm' if r.startswith('zmm') else \
                    'r64' if r.startswith('r') and not r.endswith(('d', 'w', 'b')) else 'r32' if r.startswith('e') or r.endswith('d') else \
                    'r16' if len(r) == 2 and r[1] in 'xip' or r.endswith('w') else 'r8' if r.endswith(('l', 'b', 'h')) else 'seg'
            elif '(' in o or o.startswith('0x') or o[:1].isdigit() or o.startswith('*'): k = 'mem'
            else: k = 'other'
        elif arch == 'arm64':
            if o.startswith('#'): k = 'imm'
            elif o.startswith('['): k = 'mem'
            elif re.match(r'^[xw](\d+|zr)$|^sp$', o): k = o[0] if o != 'sp' else 'x'
            elif re.match(r'^v\d+\.\w+', o): k = 'v.' + o.split('.')[1]
            elif re.match(r'^[qdshb]\d+$', o): k = o[0]
            else: k = 'other'
        else:
            k = 'imm' if re.match(r'^-?(0x)?[0-9a-f]+$', o) else 'mem' if '(' in o else 'f' if re.match(r'^f[tsa]\d+$', o) else 'r'
        kinds.append(k)
    return mn + ' ' + ','.join(kinds)

counts = collections.Counter()
forms = collections.Counter()
cls = collections.Counter()
for f in files:
    extra = ['-M', 'no-aliases'] if arch != 'amd64' and '--aliases' not in sys.argv else []
    out = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', '--no-leading-addr'] + extra + [f], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if not line.startswith((' ', '\t')): continue
        t = line.strip()
        if not t or t.endswith(':') or t.startswith('<') or t.startswith('...'): continue
        t = re.sub(r'\s*[#<].*$', '', t)
        if not t: continue
        parts = t.split(None, 1)
        mn = parts[0]
        if mn in ('<unknown>', '.word', '.long', '(bad)', 'udf'): continue
        # x86 prefixes printed as words
        while parts[0] in ('lock', 'rep', 'repne', 'repe', 'data16', 'notrack', 'bnd', 'cs', 'ds', 'rex64') and len(parts) > 1:
            pre = parts[0]
            parts = parts[1].split(None, 1)
            if not parts: break
            mn = pre + ' ' + parts[0]
        if not parts: continue
        ops = parts[1] if len(parts) > 1 else ''
        counts[mn] += 1
        forms[form(arch, mn.split()[-1], ops)] += 1
        cls[classify(arch, mn.split()[-1], ops)] += 1

total = sum(counts.values())
def cover(p):
    acc = 0
    for i, (_, n) in enumerate(counts.most_common()):
        acc += n
        if acc >= p * total: return i + 1
    return len(counts)
print('%s: %d instructions, %d distinct mnemonics, %d distinct forms; 99%% covered by %d mnemonics, 99.9%% by %d' %
      (arch, total, len(counts), len(forms), cover(0.99), cover(0.999)))
print('  integer %.1f%%, scalar floating point %.1f%%, vector %.1f%%' % tuple(100.0 * cls[k] / total for k in ('int', 'fp', 'vector')))
print('  top 15: ' + ', '.join('%s %.1f%%' % (m, 100.0 * n / total) for m, n in counts.most_common(15)))
