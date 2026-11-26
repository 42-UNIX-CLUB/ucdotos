# UC.OS

UNIX CLUB dot OS: an operating system written from scratch in C that runs in a
web browser. Made by the 42 UNIX Club at 42 Lisboa.

The kernel is one WebAssembly module. Every program is its own WebAssembly
module with its own memory. A system call is a direct call from the program's
module into the kernel's, with no JavaScript in between. The window manager,
terminal emulator, font rasterizer, shell and tools are all ordinary programs
written in C for this kernel.

## Build and run

Needs `clang` and `wasm-ld` (LLVM with the wasm32 target), `llvm-ar`, `make`,
`tar`, and Node 24 or newer for the tests. No Emscripten, no external libc.

```sh
make dist                              # builds build/dist: a folder any static web server can serve
cd build/dist && python3 -m http.server 8000
```

Open http://localhost:8000 in Chrome or Edge 137+, Firefox 153+ or Safari 27+.

| Keys | Action |
|---|---|
| Alt+Enter | new terminal |
| Alt+d | launcher |
| Alt+j / Alt+k | focus next / previous window |
| Alt+1..4 | switch workspace |
| Alt+t | switch theme |
| Alt+Shift+q | close window |

## Tests

```sh
node tools/run.mjs --init=/bin/ktest                    # kernel
node tools/run.mjs --init=/bin/libctest                 # C library
node tools/run.mjs --init=/bin/sh --input='sh /usr/share/tests/sh-tests.sh\nexit\n'   # shell and tools
node tools/shot.mjs --script=tools/tests/desktop.mjs    # boots the desktop headless, writes screenshots
```

## Layout

| Path | What |
|---|---|
| `kernel/` | the kernel |
| `abi/abi.h` | system call numbers and structures shared by kernel and programs (Linux's generic numbering) |
| `boot/` | the page and the loader: the only JavaScript, playing the role of firmware |
| `libc/`, `lib/` | the C library |
| `libgfx/` | TrueType rasterizer and 2D drawing |
| `libui/` | window protocol client and themes |
| `bin/` | programs: `sh`, core tools, `wm`, `bar`, `launcher`, `term`, `vi`, `htop`, `fetch`, `bc`, `doom` |
| `rootfs/` | files copied into the system image: dotfiles, themes, fonts, man pages, the club's pages |
| `tools/` | test runners |
| `docs/` | how it works, how to develop it, what has been decided, current status |
| `CLAUDE.md`, `AGENTS.md` | context for coding agents; start here if you are one |
| `CONTRIBUTING.md` | how to name branches, write commits and open pull requests |

## Licence

UC.OS is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 2 of the License, or (at your option) any later
version. See [LICENSE](LICENSE). Source files carry
`SPDX-License-Identifier: GPL-2.0-or-later`.

Third-party parts keep their own terms:

- `bin/doom/` contains the Doom engine from doomgeneric (GPL-2.0-or-later); see `bin/doom/README` and `bin/doom/COPYING`. No game data is included.
- `rootfs/usr/share/fonts/` contains Fira Code and JetBrains Mono (SIL Open Font License); the licence texts are in `rootfs/usr/share/licenses/`.

The "42" glyphs in the club mark are the logo of 42 and are not covered by this licence.
