#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Independent check of a solver result. Reads the Packages files and the
# output of `apt-get -s install ...` (the "Inst NAME (VERSION ...)" lines), and
# verifies that the set of packages it would install:
#   - satisfies every Depends and Pre-Depends group of every member
#     (by name and version, or by a Provides entry, versioned if needed),
#   - violates no Conflicts or Breaks between members,
#   - has one version per package name.
# It shares no code with libpkg (version comparison comes from version-oracle.py).
# Usage: check-plan.py ARCH PLAN_LOG PACKAGES_FILE...
import os, re, sys
sys.dont_write_bytecode = True  # no __pycache__ in the source tree
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
spec = importlib.util.spec_from_file_location('oracle', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'version-oracle.py'))
oracle = importlib.util.module_from_spec(spec); spec.loader.exec_module(oracle)
vercmp = oracle.vercmp

def stanzas(path):
    cur, last = {}, None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if not line.strip():
                if cur: yield cur
                cur, last = {}, None
            elif line[0] in ' \t':
                if last: cur[last] += '\n' + line
            else:
                k, _, v = line.partition(':')
                last = k.lower(); cur[last] = v.strip()
    if cur: yield cur

REL = re.compile(r'^\s*([a-z0-9][a-z0-9+.-]*)(?::([a-z0-9-]+))?\s*(?:\(\s*(<<|<=|=|>=|>>|<|>)\s*([^)\s]+)\s*\))?\s*$')
def parse(field):
    out = []
    for group in (field or '').replace('\n', ' ').split(','):
        if not group.strip(): continue
        alts = []
        for a in group.split('|'):
            m = REL.match(a)
            if not m: raise ValueError('cannot parse %r' % a)
            alts.append(m.groups())
        out.append(alts)
    return out

def sat(have, op, want):
    if not op: return True
    c = vercmp(have, want)
    return {'<<': c < 0, '<': c <= 0, '<=': c <= 0, '=': c == 0, '>=': c >= 0, '>': c >= 0, '>>': c > 0}[op]

def main():
    arch, plan = sys.argv[1], sys.argv[2]
    idx = {}
    for p in sys.argv[3:]:
        for s in stanzas(p):
            idx[(s['package'], s['version'])] = s
    chosen = {}
    for line in open(plan):
        m = re.match(r'^Inst (\S+) (?:\[\S+\] )?\((\S+) ', line)
        if m:
            if m.group(1) in chosen: print('two versions of', m.group(1)); sys.exit(1)
            chosen[m.group(1)] = m.group(2)
    members = {n: idx[(n, v)] for n, v in chosen.items()}
    provides = {}
    for n, s in members.items():
        for g in parse(s.get('provides')):
            name, _, op, ver = g[0]
            provides.setdefault(name, []).append((n, ver if op == '=' else None))
    def satisfied(alt, dependent):
        name, q, op, ver = alt
        if name in members:
            s = members[name]
            ma = s.get('multi-arch', '')
            a = s.get('architecture')
            arch_ok = a in ('all', arch) and (q in (None, 'native', arch) or (q == 'any' and ma in ('allowed', 'foreign')) or a == 'all')
            if arch_ok and sat(chosen[name], op, ver): return True
        for prov, pver in provides.get(name, []):
            if prov == dependent and False: continue
            if not op or (pver and sat(pver, op, ver)): return True
        return False
    bad = 0
    for n, s in members.items():
        for f in ('pre-depends', 'depends'):
            for g in parse(s.get(f)):
                if not any(satisfied(a, n) for a in g):
                    print('UNSATISFIED: %s %s: %s' % (n, f, g)); bad += 1
        for f in ('conflicts', 'breaks'):
            for g in parse(s.get(f)):
                name, q, op, ver = g[0]
                if name in members and name != n and sat(chosen[name], op, ver):
                    print('CONFLICT: %s %s %s %s' % (n, f, name, chosen[name])); bad += 1
                for prov, pver in provides.get(name, []):
                    if prov != n and (not op or (pver and sat(pver, op, ver))):
                        print('CONFLICT via Provides: %s %s %s (provided by %s)' % (n, f, name, prov)); bad += 1
    print('check-plan: %d packages, %d problems' % (len(members), bad))
    sys.exit(1 if bad or not members else 0)

if __name__ == '__main__':
    main()
