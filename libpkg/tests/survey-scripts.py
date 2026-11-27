#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# OPTIONAL survey of what real maintainer scripts call. It DOWNLOADS, with the
# host's curl, only the first part of each .deb (HTTP range requests: the ar
# header, debian-binary and control.tar, usually a few KiB) from deb.debian.org
# for Debian 13 (trixie) amd64, into OUT/debs/. The package list is every
# package of priority "required" (debootstrap's minbase set, plus apt) and a
# fixed list of common tools. Control files are extracted with libpkg's own
# dpkg-deb. Prints a table: command, packages whose scripts call it, calls.
# Usage: survey-scripts.py PACKAGES_FILE DPKG_DEB OUT
import collections, os, re, subprocess, sys

EXTRA = ['apt', 'hello', 'busybox-static', 'coreutils', 'bash', 'dash', 'grep', 'sed', 'gawk', 'less', 'nano', 'vim-tiny',
         'make', 'gcc', 'binutils', 'python3-minimal', 'curl', 'git', 'htop', 'nginx-light', 'sqlite3']
BASE = 'https://deb.debian.org/debian/'
KEYWORDS = set('''if then else elif fi case esac for do done while until in function { } ! [[ ]] time select'''.split())
BUILTINS = set('''echo printf test [ true false exit return shift export read unset eval exec . : set local cd pwd trap wait
umask break continue command type getopts readonly alias unalias hash'''.split())

def stanzas(path):
    cur = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if not line.strip():
            if cur: yield cur
            cur = {}
        elif line[0] not in ' \t':
            k, _, v = line.partition(':'); cur[k] = v.strip()
    if cur: yield cur

def fetch_control(url, dest):
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        return
    head = subprocess.run(['curl', '-fsSL', '-r', '0-131', url], capture_output=True, check=True).stdout
    assert head[:8] == b'!<arch>\n', url
    size1 = int(head[8 + 48:8 + 58].split()[0])
    off2 = 8 + 60 + size1 + (size1 & 1)
    size2 = int(head[off2 + 48:off2 + 58].split()[0])
    end = off2 + 60 + size2 + (size2 & 1) - 1
    data = subprocess.run(['curl', '-fsSL', '-r', '0-%d' % end, url], capture_output=True, check=True).stdout
    with open(dest, 'wb') as f:
        f.write(data)

def strip_quotes(text):
    """Blank out quoted text and comments but keep $(...) and `...` inside double quotes."""
    out, i, n = [], 0, len(text)
    state = []  # stack: 'd' double quote, 'c' command substitution inside quotes
    while i < n:
        c = text[i]
        top = state[-1] if state else None
        if top == 'd':
            if c == '\\': i += 2; continue
            if c == '"': state.pop(); out.append('""'); i += 1; continue
            if text.startswith('$(', i): state.append('c'); out.append(' $('); i += 2; continue
            if c == '\n': out.append('\n')
            i += 1; continue
        if c == '\\': out.append(text[i:i + 2]); i += 2; continue
        if c == "'":
            k = text.find("'", i + 1)
            k = n if k < 0 else k
            out.append("''" + '\n' * text.count('\n', i, k)); i = k + 1; continue
        if c == '"': state.append('d'); i += 1; continue
        if c == '#' and (i == 0 or text[i - 1] in ' \t\n;'):
            k = text.find('\n', i); i = n if k < 0 else k; continue
        if top == 'c' and c == ')': state.pop(); out.append(')'); i += 1; continue
        if top == 'c' and c == '(': state.append('c'); out.append('('); i += 1; continue
        out.append(c); i += 1
    return ''.join(out)

def commands(script):
    text = re.sub(r'\\\n', ' ', script)
    text = strip_quotes(text)
    text = re.sub(r'(?m)^\s*[\w|*.\-"\[\]/ ]+\)\s*$', '', text)   # case labels on their own line
    text = re.sub(r'(?m)^(\s*)[\w|*.\-"\[\]/ ]+\)\s+', r'\1', text)  # case labels before a command
    out = []
    in_heredoc = None
    for raw in text.split('\n'):
        line = raw.strip()
        if in_heredoc:
            if line == in_heredoc: in_heredoc = None
            continue
        m = re.search(r'<<-?\s*[\'"]?(\w+)[\'"]?', line)
        if m: in_heredoc = m.group(1)
        if not line or line.startswith('#'): continue
        line = re.sub(r"'[^']*'", "''", line)
        line = re.sub(r'#.*$', '', line) if ' #' in line or line.startswith('#') else line
        for part in re.split(r'\|\||&&|;|\||\$\(|`|\(|\)', line):
            words = part.strip().split()
            while words and (re.match(r'^\w+=', words[0]) or words[0] in KEYWORDS or words[0] in ('!',)):
                words = words[1:]
            if not words: continue
            w = words[0].strip('"')
            if w.endswith(')') or w.startswith('$') or w.startswith('-') or '=' in w or w.startswith('"') or not re.match(r'^[A-Za-z0-9_./+-]+$', w):
                continue
            if w in ('then', 'do', 'else', 'esac', 'fi', 'done', ';;'): continue
            if not re.match(r'^[a-z/._]', w) or w.isdigit(): continue
            if re.match(r'^[a-z_][a-z0-9_]*$', w) and w + '()' in script.replace(' ()', '()'):
                continue  # a shell function defined in the script
            out.append(os.path.basename(w) if w.startswith('/') else w)
    return out

def main():
    packages_file, dpkg_deb, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(os.path.join(out, 'debs'), exist_ok=True)
    idx = {s['Package']: s for s in stanzas(packages_file) if 'Package' in s}
    names = sorted(n for n, s in idx.items() if s.get('Priority') == 'required')
    names += [n for n in EXTRA if n not in names and n in idx]
    per_cmd = collections.Counter(); pkgs_cmd = collections.defaultdict(set)
    interps = collections.Counter(); triggers = collections.Counter(); scripts_n = 0; with_scripts = 0
    for n in names:
        s = idx[n]
        deb = os.path.join(out, 'debs', os.path.basename(s['Filename']))
        fetch_control(BASE + s['Filename'], deb)
        cdir = os.path.join(out, 'control', n)
        subprocess.run([dpkg_deb, '-e', deb, cdir], check=True, capture_output=True)
        has = False
        for sc in ('preinst', 'postinst', 'prerm', 'postrm', 'config'):
            p = os.path.join(cdir, sc)
            if not os.path.exists(p): continue
            body = open(p, encoding='utf-8', errors='replace').read()
            has = True; scripts_n += 1
            first = body.split('\n', 1)[0]
            interps[first.strip() if first.startswith('#!') else '(no #! line)'] += 1
            for c in commands(body):
                per_cmd[c] += 1; pkgs_cmd[c].add(n)
        if has: with_scripts += 1
        t = os.path.join(cdir, 'triggers')
        if os.path.exists(t):
            for line in open(t):
                w = line.split()
                if len(w) >= 2 and not w[0].startswith('#'): triggers['%s %s' % (w[0], w[1])] += 1
    print('packages surveyed: %d (%d with maintainer scripts, %d scripts)' % (len(names), with_scripts, scripts_n))
    print('packages: ' + ' '.join(names))
    print('\ninterpreters:')
    for k, v in interps.most_common(): print('  %4d  %s' % (v, k))
    print('\n%-28s %8s %6s  %s' % ('command', 'packages', 'calls', 'kind'))
    for c, v in sorted(per_cmd.items(), key=lambda kv: (-len(pkgs_cmd[kv[0]]), -kv[1], kv[0])):
        kind = 'shell builtin' if c in BUILTINS else ''
        print('%-28s %8d %6d  %s' % (c, len(pkgs_cmd[c]), v, kind))
    print('\ntriggers declared:')
    for k, v in triggers.most_common(): print('  %4d  %s' % (v, k))

if __name__ == '__main__':
    main()
