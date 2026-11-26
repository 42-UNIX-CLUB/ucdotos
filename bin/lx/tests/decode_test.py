#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Decoder test against an independent reference: LLVM's RISC-V disassembler and assembler.
#   1. every 16-bit compressed encoding, and N random 32-bit words drawn from the RV64GC
#      major opcodes, are disassembled by llvm-objdump (which prints compressed
#      instructions in their expanded form);
#   2. that text is assembled again by llvm-mc without the C extension, giving the 32-bit
#      instruction LLVM thinks it is;
#   3. our decoder's result for the original word must equal its result for LLVM's word,
#      field by field (only the fields the format uses), and both must agree on validity.
# Usage: decode_test.py OUTDIR [N]   (OUTDIR holds the built decode_test helper)
import os, random, re, subprocess, sys

out = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 60000
helper = os.path.join(out, 'decode_test')
MATTR = '+m,+a,+f,+d,+c,+zicsr,+zifencei'
MATTR_NOC = '+m,+a,+f,+d,+zicsr,+zifencei'

def run(cmd, inp=None):
    return subprocess.run(cmd, input=inp, capture_output=True, text=True)

random.seed(12345)
words = [h for h in range(0x10000) if (h & 3) != 3]
majors = [0x03, 0x07, 0x0f, 0x13, 0x17, 0x1b, 0x23, 0x27, 0x2f, 0x33, 0x37, 0x3b, 0x43, 0x47, 0x4b,
          0x4f, 0x53, 0x63, 0x67, 0x6f, 0x73]
w32 = []
for i in range(N):
    w = (random.getrandbits(25) << 7) | random.choice(majors)
    if random.random() < 0.5 and (w & 0x7f) in (0x33, 0x3b, 0x53, 0x2f):
        # keep funct7 / funct5 in the defined range more often
        f7 = random.choice([0x00, 0x01, 0x20, 0x02, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0c, 0x0d, 0x10, 0x11,
                            0x14, 0x15, 0x20, 0x21, 0x2c, 0x2d, 0x50, 0x51, 0x60, 0x61, 0x68, 0x69, 0x70, 0x71, 0x78, 0x79])
        w = (w & 0x01ffffff) | (f7 << 25)
    w32.append(w)

# 1. disassemble with llvm-objdump (instructions laid out back to back; address known)
src = ['.text']
addr = 0
table = []  # (address, word, length)
for h in words:
    src.append('.insn 2, 0x%04x' % h); table.append((addr, h, 2)); addr += 2
for w in w32:
    src.append('.insn 4, 0x%08x' % w); table.append((addr, w, 4)); addr += 4
open(os.path.join(out, 'dt.s'), 'w').write('\n'.join(src) + '\n')
r = run(['llvm-mc', '-triple=riscv64', '-mattr=' + MATTR, '-filetype=obj', '-o', os.path.join(out, 'dt.o'), os.path.join(out, 'dt.s')])
if r.returncode: print(r.stderr); sys.exit(1)
r = run(['llvm-objdump', '-d', '--mattr=' + MATTR, os.path.join(out, 'dt.o')])
dis = {}
for line in r.stdout.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{2,8} ?)+)\s+(.*)$', line)
    if not m: continue
    dis[int(m.group(1), 16)] = m.group(3).strip()

# 2. our decoder on the originals
ours = run([helper], '\n'.join('%x' % w for _, w, _ in table) + '\n').stdout.splitlines()

# 3. reassemble LLVM's text without C
asm = ['.text']
idx = []
for i, (a, w, n) in enumerate(table):
    t = dis.get(a)
    if t is None or t.startswith('<unknown>') or t.startswith('unimp'):
        continue
    # branch and jump targets are printed as absolute addresses: make them offsets
    t = re.sub(r'\s*<[^>]*>', '', t)
    mm = re.match(r'^(\S+)\s+(.*)$', t)
    if mm:
        op, args = mm.group(1), mm.group(2)
        if op.startswith('b') or op in ('j', 'jal'):
            parts = [p.strip() for p in args.split(',')]
            if parts and re.match(r'^0x[0-9a-f]+$', parts[-1]):
                parts[-1] = str(int(parts[-1], 16) - a)
                t = op + ' ' + ', '.join(parts)
    asm.append('L%d:' % i)
    asm.append(t)
    idx.append(i)
open(os.path.join(out, 'dt2.s'), 'w').write('\n'.join(asm) + '\n')
r = run(['llvm-mc', '-triple=riscv64', '-mattr=' + MATTR_NOC, '-show-encoding', os.path.join(out, 'dt2.s')])
enc = {}
cur = None
for line in r.stdout.splitlines():
    m = re.match(r'^L(\d+):', line.strip())
    if m: cur = int(m.group(1)); continue
    m = re.search(r'encoding: \[([^\]]*)\]', line)
    if m and cur is not None:
        b = [int(x, 16) for x in m.group(1).split(',')]
        enc[cur] = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24
        cur = None
theirs_in = [enc[i] for i in idx if i in enc]
theirs = dict(zip([i for i in idx if i in enc], run([helper], '\n'.join('%x' % w for w in theirs_in) + '\n').stdout.splitlines()))

FMT = {}
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rv.h')):
    for m in re.finditer(r'X\((\w+), (\w), ', line):
        FMT[m.group(1)] = m.group(2)
FIELDS = {'R': (1, 2, 3), 'I': (1, 2, 7), 'L': (1, 2, 7), 'S': (2, 3, 7), 'B': (2, 3, 7), 'U': (1, 7), 'J': (1, 7),
          'F': (1, 2, 3, 4, 5), 'C': (1, 2, 7), 'A': (1, 2, 3, 5), 'X': ()}
FP_NORS = ('FSQRT', 'FCVT', 'FMV', 'FCLASS')
def rv_writes(op):
    return FMT.get(op) in ('R', 'I', 'U', 'J', 'L')

fails = 0
checked = 0
illegal_disagree = 0
for i, (a, w, n) in enumerate(table):
    o = ours[i].split()
    t = dis.get(a, '<missing>')
    llvm_invalid = t.startswith('<unknown>') or t.startswith('unimp') or t == '<missing>'
    # The ISA says implementations ignore FENCE/FENCE.I's unused fields; LLVM rejects
    # some of those encodings. Both views are acceptable: not counted.
    if o[0] in ('FENCE', 'FENCE_I') and llvm_invalid:
        continue
    # HINT encodings (e.g. c.addi x0, imm): LLVM prints the compressed form; they must
    # decode to an instruction whose only effect is a write to x0.
    if t.startswith('c.') and o[0] != 'ILLEGAL':
        if o[1] == '0' and rv_writes(o[0]): continue
        if o[0] in ('ADDI', 'SLLI', 'SRLI', 'SRAI') and o[1] == o[2] and o[7] == '0': continue
        if fails < 30: print('hint not a no-op: %0*x "%s" ours %s' % (n * 2, w, t, ours[i]))
        fails += 1
        continue
    if o[0] == 'ILLEGAL' or llvm_invalid:
        if (o[0] == 'ILLEGAL') != llvm_invalid:
            illegal_disagree += 1
            if illegal_disagree <= 15: print('validity: %0*x ours=%s llvm=%s' % (n * 2, w, o[0], t))
        continue
    if i not in theirs:
        if fails < 30: print('llvm could not reassemble: %0*x "%s"' % (n * 2, w, t))
        fails += 1
        continue
    th = theirs[i].split()
    checked += 1
    # c.mv is defined as add rd, x0, rs2; LLVM's "mv" text is addi rd, rs, 0: the same.
    def canon(f):
        f = list(f)
        if f[0] == 'ADD' and f[2] == '0': f = ['ADDI', f[1], f[3], '0', f[4], f[5], f[6], '0']
        return f
    o = canon(o); th = canon(th)
    op = o[0]
    flds = FIELDS[FMT.get(op, 'R')]
    if FMT.get(op) == 'F':
        flds = (1, 2, 5) if op.startswith(FP_NORS) else (1, 2, 3, 5)
        if op.startswith(('FMADD', 'FMSUB', 'FNMSUB', 'FNMADD')): flds = (1, 2, 3, 4, 5)
        if op.startswith(('FSGNJ', 'FMIN', 'FMAX', 'FEQ', 'FLT', 'FLE')): flds = (1, 2, 3)
        if op.startswith(('FMV', 'FCLASS')): flds = (1, 2)
    if op in ('FENCE',): flds = ()
    if op != th[0] or any(o[k] != th[k] for k in flds):
        fails += 1
        if fails <= 30: print('mismatch %0*x "%s": ours %s / llvm word %08x -> %s' % (n * 2, w, t, ours[i], theirs_in[list(theirs).index(i)] if i in theirs else 0, theirs[i]))
print('decoder: %d encodings compared with LLVM, %d mismatches, %d validity disagreements' % (checked, fails, illegal_disagree))
sys.exit(1 if fails or illegal_disagree else 0)
