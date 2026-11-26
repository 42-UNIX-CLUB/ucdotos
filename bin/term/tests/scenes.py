#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Render test scenes natively through vt.c + render.c and save PNGs.
Run from the repository root:  python3 bin/term/tests/scenes.py BUILD_DIR RENDER_TEST_BINARY"""
import os, subprocess, sys
from PIL import Image

build, binary = sys.argv[1], sys.argv[2]
sdir, odir = f'{build}/scenes', f'{build}/shots/native'
os.makedirs(sdir, exist_ok=True)
os.makedirs(odir, exist_ok=True)
E = '\x1b'

def prompt(cmd=''):
    return f'{E}[1;32muser@ucos{E}[0m:{E}[1;34m~{E}[0m$ {cmd}'

ls = subprocess.run('ls --color=always -la /usr/bin | head -22', shell=True, capture_output=True).stdout
ls = ls.replace(b'\n', b'\r\n')
scenes = {}
scenes['prompt_ls'] = prompt('ls --color -la /usr/bin | head -22\r\n').encode() + ls + prompt().encode()

box = (
    '┌──────────┬────────┬───────┐\r\n'
    '│ Process  │  PID   │  CPU  │\r\n'
    '├──────────┼────────┼───────┤\r\n'
    '│ sh       │      2 │  0.1% │\r\n'
    '│ term     │      1 │  3.4% │\r\n'
    '╞══════════╪════════╪═══════╡\r\n'
    '│ total    │      2 │  3.5% │\r\n'
    '└──────────┴────────┴───────┘\r\n'
    '╭─ rounded ─╮  ┏━ heavy ━┓  ╔═ double ═╗\r\n'
    '│ corners   │  ┃ lines   ┃  ║ lines    ║\r\n'
    '╰───────────╯  ┗━━━━━━━━━┛  ╚══════════╝\r\n'
    f'{E}(0lqqqqwqqqqk{E}(B  DEC special graphics\r\n'
    f'{E}(0x    x    x{E}(B\r\n'
    f'{E}(0tqqqqnqqqqu{E}(B\r\n'
    f'{E}(0mqqqqvqqqqj{E}(B\r\n'
    'blocks: █▉▊▋▌▍▎▏ ▁▂▃▄▅▆▇█ ░▒▓ ▖▗▘▙▚▛▜▝▞▟ ▀▐▔▕\r\n'
    f'{E}[44mpowerline:{E}[34;42m{E}[30m main {E}[32;49m{E}[0m  {E}[7m round {E}[0m\r\n'
)
scenes['box'] = box.encode()

pal = '16 colours (fg, then bg):\r\n'
for i in range(16):
    pal += f'{E}[{30 + i if i < 8 else 82 + i}m c{i:<2} '
pal += f'{E}[0m\r\n'
for i in range(16):
    pal += f'{E}[{40 + i if i < 8 else 92 + i}m c{i:<2} '
pal += f'{E}[0m\r\n\r\n256 colours:\r\n'
for row in range(16):
    for col in range(16):
        n = row * 16 + col
        pal += f'{E}[48;5;{n}m{n:4d}'
    pal += f'{E}[0m\r\n'
pal += '\r\ntruecolor (; and : forms):\r\n'
for x in range(80):
    r, g, b = int(255 * x / 79), int(255 * (1 - abs(x - 40) / 40)), int(255 * (1 - x / 79))
    pal += f'{E}[48;2;{r};{g};{b}m ' if x % 2 else f'{E}[48:2::{r}:{g}:{b}m '
pal += f'{E}[0m\r\n'
scenes['palette'] = pal.encode()

# half blocks: two pixels per cell, fg = top, bg = bottom, smooth 2D gradient
hb = ''
rows, cols = 24, 80
for y in range(rows):
    for x in range(cols):
        def c(py):
            return (int(255 * x / (cols - 1)), int(255 * py / (2 * rows - 1)), int(255 - 255 * x / (cols - 1)))
        t, b = c(2 * y), c(2 * y + 1)
        hb += f'{E}[38;2;{t[0]};{t[1]};{t[2]}m{E}[48;2;{b[0]};{b[1]};{b[2]}m▀'
    hb += f'{E}[0m' + ('\r\n' if y < rows - 1 else '')
scenes['halfblock'] = hb.encode()
scenes['halfblock_cursorless'] = (hb + f'{E}[?25l').encode()

attrs = (
    'Regular text: The quick brown fox jumps over the lazy dog 0123456789\r\n'
    f'{E}[1mBold text:    The quick brown fox jumps over the lazy dog 0123456789{E}[0m\r\n'
    f'{E}[2mDim text{E}[0m  {E}[3mitalic (no italic font){E}[0m  {E}[4munderline{E}[0m  {E}[9mstrikethrough{E}[0m  {E}[7minverse{E}[0m\r\n'
    f'{E}[1;4;31mbold underline red{E}[0m {E}[1;7;32m bold inverse green {E}[0m {E}[8mhidden{E}[0m<-hidden\r\n'
    f'{E}[1;33mbold yellow{E}[22m normal yellow {E}[39mdefault\r\n'
    'Unicode: café ñ ü λ → ✓ ★ ♥ € ∑ ∞ ≠ ≤ 中文 😀 (wide chars use one cell)\r\n'
    'Ligature candidates (drawn as separate cells): -> => != === <= >= ::\r\n'
    'Replacement: \xff\xfe bad bytes\r\n'
).encode('utf-8', 'surrogateescape')
scenes['attrs'] = attrs.replace('\xff\xfe'.encode(), b'\xff\xfe')

cursor = prompt('echo hello').encode()

def run(name, data, theme='deck', extra=(), cols=80, rows=24):
    path = f'{sdir}/{name}.bin'
    open(path, 'wb').write(data)
    ppm = f'{sdir}/{name}_{theme}.ppm'
    subprocess.run([binary, '--theme', theme, '--cols', str(cols), '--rows', str(rows), '--fontdir', 'rootfs/usr/share/fonts', '--out', ppm, *extra, path], check=True)
    png = f'{odir}/{name}_{theme}.png'
    Image.open(ppm).save(png)
    print(png)

for theme in ('deck', 'brand'):
    run('prompt_ls', scenes['prompt_ls'], theme, cols=100, rows=26)
    run('box', scenes['box'], theme, rows=18)
    run('palette', scenes['palette'], theme, cols=80, rows=24)
    run('attrs', scenes['attrs'], theme, rows=9)
    run('halfblock', scenes['halfblock_cursorless'], theme)
    run('cursor_focused', cursor, theme, cols=40, rows=3)
    run('cursor_unfocused', cursor, theme, ('--unfocused',), cols=40, rows=3)
run('cursor_bar', cursor + b'\x1b[5 q', 'deck', cols=40, rows=3)
run('cursor_underline', cursor + b'\x1b[3 q', 'deck', cols=40, rows=3)
run('big_font', scenes['box'], 'deck', ('--font-size', '28'), cols=50, rows=18)
