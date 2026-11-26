#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Compare our reading of ExecStart=/ExecStartPre=/ExecReload=/ExecStop= with what the
# host's systemd reports (systemctl show; read only, changes nothing on the host).
# usage: compare-systemctl.py TEST_SVC_BINARY UNITFILE...   (non-template .service files)
import subprocess, sys, re, os
tool, files = sys.argv[1], sys.argv[2:]
keys = ['ExecStart', 'ExecStartPre', 'ExecStartPost', 'ExecReload', 'ExecStop']
names = {}
for f in files:
    n = os.path.basename(f)
    if '@' in n or n in names:
        continue
    names[n] = f
units = sorted(names)
ours, theirs = {}, {}
for k in keys:
    out = subprocess.run(['systemctl', 'show', '-p', 'Id', '-p', 'FragmentPath', '-p', 'DropInPaths', '-p', k] + units,
                         capture_output=True, text=True).stdout
    for block in out.split('\n\n'):
        d = {}
        for line in block.split('\n'):
            if '=' in line:
                a, _, b = line.partition('=')
                d.setdefault(a, []).append(b)
        if 'Id' not in d:
            continue
        uid = d['Id'][0]
        if uid not in names or d.get('FragmentPath', [''])[0] != names[uid] or d.get('DropInPaths', [''])[0]:
            continue  # aliases, overridden or drop-in units: not comparable with the fragment alone
        cmds = []
        for v in d.get(k, []):
            for m in re.finditer(r'path=(\S*) ; argv\[\]=(.*?) ; ignore_errors=(\w+)', v):
                cmds.append('path=%s ; argv[]=%s ; ignore_errors=%s' % m.groups())
        theirs[(uid, k)] = cmds
for (uid, k) in theirs:
    out = subprocess.run([tool, 'show', names[uid], k], capture_output=True, text=True).stdout
    ours[(uid, k)] = [l for l in out.split('\n') if l]
same = diff = nonempty = 0
for key in sorted(theirs):
    if ours.get(key) == theirs[key]:
        same += 1; nonempty += bool(theirs[key])
    else:
        diff += 1
        print('DIFF', key)
        print('  systemd:', theirs[key])
        print('  ours:   ', ours.get(key))
print("compared %d (unit, key) pairs over %d units: %d equal (%d with commands), %d different" % (same + diff, len({u for u, _ in theirs}), same, nonempty, diff))
