#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# A second, separate reading of deb-version(7), used to fuzz libpkg's C code.
# It shares no code with it: it splits each part into alternating runs and
# compares those, as the man page describes. Usage:
#   python3 version-oracle.py UNIT_BINARY [COUNT]
# It generates random version pairs, asks `UNIT_BINARY --vercmp` for answers,
# and reports any disagreement. Exits 1 on a mismatch.
import random, subprocess, sys

def runs(s):
    """Alternating non-digit / digit runs, always starting with a non-digit run."""
    out, i = [], 0
    while i < len(s) or not out:
        j = i
        while j < len(s) and not s[j].isdigit():
            j += 1
        out.append(s[i:j])
        k = j
        while k < len(s) and s[k].isdigit():
            k += 1
        out.append(s[j:k])
        i = k
        if i >= len(s):
            break
    return out

def weight(c):
    if c == '~':
        return -1
    if c.isalpha():
        return ord(c)
    return ord(c) + 256

def cmp_text(a, b):
    # character by character; the end of a string weighs 0, "~" -1
    for i in range(max(len(a), len(b))):
        wa = weight(a[i]) if i < len(a) else 0
        wb = weight(b[i]) if i < len(b) else 0
        if wa != wb:
            return -1 if wa < wb else 1
    return 0

def cmp_part(a, b):
    ra, rb = runs(a), runs(b)
    n = max(len(ra), len(rb))
    ra += [''] * (n - len(ra))
    rb += [''] * (n - len(rb))
    for k in range(n):
        if k % 2 == 0:
            c = cmp_text(ra[k], rb[k])
        else:
            x, y = int(ra[k] or '0'), int(rb[k] or '0')
            c = (x > y) - (x < y)
        if c:
            return c
    return 0

def split(v):
    epoch = '0'
    if ':' in v:
        epoch, v = v.split(':', 1)
    rev = ''
    if '-' in v:
        v, rev = v.rsplit('-', 1)
    return int(epoch or '0'), v, rev

def vercmp(a, b):
    ea, ua, ra = split(a)
    eb, ub, rb = split(b)
    if ea != eb:
        return -1 if ea < eb else 1
    return cmp_part(ua, ub) or cmp_part(ra, rb)

def rand_part(r, first_digit):
    alphabet = '0123456789' * 3 + 'abcz' + 'AZ' + '.+~' + '~'
    s = r.choice('0123456789') if first_digit else ''
    for _ in range(r.randint(0, 7)):
        s += r.choice(alphabet)
    return s

def rand_version(r):
    v = ''
    if r.random() < 0.2:
        v += str(r.randint(0, 3)) + ':'
    v += rand_part(r, True)
    if r.random() < 0.6:
        rev = r.choice('0123456789abc') + rand_part(r, False)
        v += '-' + rev
    return v

def main():
    unit = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    r = random.Random(1234)
    pairs = []
    for _ in range(count):
        a = rand_version(r)
        b = a if r.random() < 0.05 else (rand_version(r) if r.random() < 0.5 else mutate(r, a))
        if ':' in b and not b.split(':', 1)[0].isdigit():
            b = a  # a mutation broke the epoch; such strings are invalid versions
        pairs.append((a, b))
    inp = ''.join(f'{a} {b}\n' for a, b in pairs)
    out = subprocess.run([unit, '--vercmp'], input=inp, capture_output=True, text=True, check=True).stdout.split()
    bad = 0
    for (a, b), got in zip(pairs, out):
        want = vercmp(a, b)
        if int(got) != want:
            bad += 1
            if bad <= 20:
                print(f'MISMATCH {a!r} {b!r}: C says {got}, oracle says {want}')
    print(f'version-oracle: {len(pairs)} pairs, {bad} mismatches')
    sys.exit(1 if bad or len(out) != len(pairs) else 0)

def mutate(r, a):
    i = r.randint(0, len(a))
    return a[:i] + r.choice(['~', '0', '1', 'a', '.', '+', '~~', '9', '']) + a[i:]

if __name__ == '__main__':
    main()
