#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Conformance against real programs: record their output under a pty with
scripted keys, replay it through vt.c (tests/replay.c) and through pyte, and
compare the screens cell by cell (text, colours, attributes, cursor) at every
checkpoint.

  python3 bin/term/tests/conformance.py BUILD_DIR REPLAY_BINARY [--no-record]

Needs pyte (pip install pyte). Run from the repository root."""
import json, os, pty, select, struct, subprocess, sys, time, fcntl, termios, signal
import pyte

build, replay = sys.argv[1], sys.argv[2]
record = '--no-record' not in sys.argv
out = f'{build}/conformance'
os.makedirs(out, exist_ok=True)
COLS, ROWS = 80, 24

def run_session(name, argv, steps, cols=COLS, rows=ROWS, env_extra=None):
    """steps: list of (delay_seconds, bytes_to_send or None). A checkpoint is taken
    after each step's delay, before sending its bytes."""
    env = dict(os.environ, TERM='xterm-256color', COLUMNS=str(cols), LINES=str(rows), LANG='C.UTF-8', LC_ALL='C.UTF-8')
    env.pop('COLORTERM', None)
    if env_extra: env.update(env_extra)
    pid, fd = pty.fork()
    if pid == 0:
        os.execvpe(argv[0], argv, env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    data, checkpoints = bytearray(), []
    def pump(secs):
        end = time.time() + secs
        while True:
            left = end - time.time()
            if left <= 0: break
            r, _, _ = select.select([fd], [], [], left)
            if r:
                try: chunk = os.read(fd, 65536)
                except OSError: return False
                if not chunk: return False
                data.extend(chunk)
        return True
    alive = True
    for delay, keys in steps:
        if alive: alive = pump(delay)
        checkpoints.append(len(data))
        if keys and alive:
            os.write(fd, keys)
    if alive: pump(1.0)
    try: os.kill(pid, signal.SIGKILL)
    except ProcessLookupError: pass
    os.waitpid(pid, 0)
    open(f'{out}/{name}.bin', 'wb').write(bytes(data))
    json.dump({'cols': cols, 'rows': rows, 'checkpoints': checkpoints}, open(f'{out}/{name}.json', 'w'))
    print(f'recorded {name}: {len(data)} bytes, {len(checkpoints)} checkpoints')

def sessions():
    sample = os.path.abspath('bin/term/vt.c')
    run_session('ls', ['sh', '-c', 'ls --color=always -la /usr/bin | head -100'], [(1.5, None)])
    run_session('vim', ['vim', '-u', 'DEFAULTS', '-i', 'NONE', '-N', '+syntax on', sample],
                [(1.5, b'\x06'), (0.5, b'\x06'), (0.5, b'jjjjjjjjjj'), (0.5, b'\x02'), (0.5, b'/scroll_up\r'), (0.5, b'Ohello\x1b'),
                 (0.5, b'G'), (0.5, b'gg'), (0.5, b':q!\r'), (0.8, None)])
    run_session('top', ['top', '-d', '0.5'], [(1.2, None), (0.6, None), (0.6, None), (0.6, b'q'), (0.5, None)])
    run_session('man', ['man', 'ls'], [(2.5, b' '), (0.6, b' '), (0.6, b'b'), (0.6, b'/sort\r'), (0.6, b'q'), (0.5, None)],
                env_extra={'MANPAGER': 'less', 'PAGER': 'less', 'MANWIDTH': str(COLS)})
    colours = os.path.abspath(f'{out}/colours.sh')
    with open(colours, 'w') as f:
        f.write('for i in $(seq 0 255); do printf "\\033[38;5;%dm%3d\\033[48;5;%dm#\\033[0m" $i $i $((255-i)); done; echo\n')
        f.write('for i in $(seq 0 4 255); do printf "\\033[38;2;%d;%d;%dm\\033[48;2;%d;100;%dm\\342\\226\\200" $i $((255-i)) 128 $((255-i)) $i; done; printf "\\033[0m\\n"\n')
        f.write('printf "\\033[1mbold\\033[0m \\033[4munder\\033[0m \\033[7mrev\\033[0m \\033[9mstrike\\033[0m \\033[3mital\\033[0m \\033[1;31;44mmix\\033[0m\\n"\n')
        f.write('for i in 30 31 32 33 34 35 36 37 90 91 92 93 94 95 96 97; do printf "\\033[%dm%d \\033[0m" $i $i; done; echo\n')
    run_session('colours', ['sh', colours], [(1.5, None)])

# ---------- comparison ----------

class RefScreen(pyte.Screen):
    # pyte passes private=True for CSI > ... m (XTMODKEYS, sent by vim) and then crashes
    def select_graphic_rendition(self, *attrs, private=False):
        if not private: super().select_graphic_rendition(*attrs)

NAMES = ['black', 'red', 'green', 'brown', 'blue', 'magenta', 'cyan', 'white']
def pal_hex(n):
    if n < 16: return None
    if n < 232:
        n -= 16
        lv = [0, 95, 135, 175, 215, 255]
        return '%02x%02x%02x' % (lv[n // 36], lv[(n // 6) % 6], lv[n % 6])
    g = 8 + 10 * (n - 232)
    return '%02x%02x%02x' % (g, g, g)

def ours_color(s):
    if s == '-': return 'default'
    if s[0] == 'P':
        n = int(s[1:])
        if n < 8: return NAMES[n]
        if n < 16: return 'bright' + NAMES[n - 8]
        return pal_hex(n)
    return s[1:]

def pyte_color(c):
    c = c.lower()
    if c in ('default',): return 'default'
    if c.startswith('bright'): return 'bright' + {'brown': 'brown'}.get(c[6:], c[6:])
    if c in NAMES: return c
    # pyte resolves 38;5;0..15 to fixed xterm RGB; we keep the (themed) palette index
    low16 = [h.lower() for h in pyte.graphics.FG_BG_256[:16]]
    if c in low16:
        i = low16.index(c)
        return NAMES[i] if i < 8 else 'bright' + NAMES[i - 8]
    return c  # hex

def rgb(c):
    # canonical form: 'default' or xterm RGB hex (names use xterm's default 16 colours)
    if c == 'default': return c
    full = NAMES + ['bright' + n for n in NAMES]
    if c in full: return pyte.graphics.FG_BG_256[full.index(c)].lower()
    return c

def parse_replay(text, rows):
    shots, lines, i = [], text.split('\n'), 0
    while i < len(lines):
        if lines[i].startswith('@ '):
            _, off, cx, cy, alt = lines[i].split()
            rows_text = lines[i + 1:i + 1 + rows]
            attrs = [l[2:].split(' ') for l in lines[i + 1 + rows:i + 1 + 2 * rows]]
            shots.append({'off': int(off), 'cx': int(cx), 'cy': int(cy), 'alt': int(alt), 'rows': rows_text, 'attrs': attrs})
            i += 1 + 2 * rows
        else: i += 1
    return shots

def compare(name):
    meta = json.load(open(f'{out}/{name}.json'))
    cols, rows, cps = meta['cols'], meta['rows'], meta['checkpoints']
    data = open(f'{out}/{name}.bin', 'rb').read()
    res = subprocess.run([replay, str(cols), str(rows), f'{out}/{name}.bin', *map(str, cps)], capture_output=True, text=True, errors='replace')
    ours = parse_replay(res.stdout, rows)
    screen = RefScreen(cols, rows)
    stream = pyte.ByteStream(screen)
    pos = 0
    report = {'checkpoints': 0, 'cells': 0, 'text_diff': 0, 'color_diff': 0, 'attr_diff': 0, 'cursor_diff': 0, 'examples': []}
    report['skipped'] = 0
    for shot in ours:
        off = shot['off']
        stream.feed(data[pos:off]); pos = off
        if not shot['alt'] and b'\x1b[?1049l' in data[:off]:
            report['skipped'] += 1  # back on the main screen: pyte has no alternate screen to leave
            continue
        report['checkpoints'] += 1
        for y in range(rows):
            line = screen.buffer[y]
            ours_row = shot['rows'][y] if y < len(shot['rows']) else ''
            ours_cells = list(ours_row) + [' '] * (cols - len(ours_row))
            for x in range(cols):
                pc = line[x]
                report['cells'] += 1
                pd = pc.data if pc.data else ' '
                od = ours_cells[x] if x < len(ours_cells) else ' '
                if pd != od:
                    report['text_diff'] += 1
                    if len(report['examples']) < 6: report['examples'].append(f'cp@{off} ({x},{y}) text ours={od!r} pyte={pd!r}')
                    continue
                f, b, a = shot['attrs'][y][x].split(',')
                a = int(a, 16)
                of, ob = rgb(ours_color(f)), rgb(ours_color(b))
                pf, pb = rgb(pyte_color(pc.fg)), rgb(pyte_color(pc.bg))
                if pd != ' ' and of != pf or ob != pb:
                    report['color_diff'] += 1
                    if len(report['examples']) < 12: report['examples'].append(f'cp@{off} ({x},{y}) colour ours={of}/{ob} pyte={pf}/{pb} char={pd!r}')
                oa = (bool(a & 1), bool(a & 4), bool(a & 8), bool(a & 32), bool(a & 128))
                pa = (pc.bold, pc.italics, pc.underscore, pc.reverse, pc.strikethrough)
                if pd != ' ' and oa != pa:
                    report['attr_diff'] += 1
                    if len(report['examples']) < 14: report['examples'].append(f'cp@{off} ({x},{y}) attrs ours={oa} pyte={pa}')
        px = min(screen.cursor.x, cols - 1)
        if (px, screen.cursor.y) != (shot['cx'], shot['cy']):
            report['cursor_diff'] += 1
            if len(report['examples']) < 16: report['examples'].append(f'cp@{off} cursor ours=({shot["cx"]},{shot["cy"]}) pyte=({px},{screen.cursor.y}) alt={shot["alt"]}')
    return report

names = ['ls', 'vim', 'top', 'man', 'colours']
if record: sessions()
total_bad = 0
for n in names:
    if not os.path.exists(f'{out}/{n}.bin'): continue
    r = compare(n)
    bad = r['text_diff'] + r['color_diff'] + r['attr_diff']
    total_bad += bad
    print(f"{n:8s} {r['checkpoints']:2d} checkpoints, {r['cells']:6d} cells: {r['text_diff']} text, {r['color_diff']} colour, {r['attr_diff']} attribute differences, {r['cursor_diff']} cursor differences" + (f" ({r['skipped']} checkpoints after leaving the alternate screen not compared)" if r['skipped'] else ''))
    for e in r['examples']: print('    ' + e)
print(f'total cell differences: {total_bad}')
