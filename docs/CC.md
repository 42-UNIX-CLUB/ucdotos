# The C compiler and linker: design notes

Design notes for the C compiler and linker that run inside UC.OS (`bin/cc/`, `bin/ld/`). They record the
choice of approach, what was assumed on the owners' behalf, and how each part was
verified. Everything here describes the code as it is on 2026-11-26.

## 1. How does `cc` inside UC.OS produce a runnable program?

A UC.OS program today is made by clang (WebAssembly relocatable objects) and wasm-ld
(linking against `libc.a`, `crt0_*.o`, `libgfx.a`, `libui.a`). Inside UC.OS there is no
clang and no wasm-ld. The options that were on the table:

| | What it needs | What it cannot do | Size (written here) |
|---|---|---|---|
| **A** relocatable objects in the tool-conventions "linking" format, plus our own `ld` that reads them and `ar` archives | an object writer (symbols, relocations, padded LEBs) and a linker (symbol resolution, archives, data layout, table, relocation, start file) | nothing a normal Unix toolchain does not do; no dynamic linking | object writer 415 lines, linker 1,330 lines |
| **B** one final module straight from all the C files, the libc compiled from source each time (or from a pre-digested form) | only a module writer; the libc sources (or a digest) in the image | separate compilation (`make` rebuilds everything every time), libraries, linking anything built by clang (libui, libgfx as shipped), mixing compilers | no linker, but every build compiles the libc (about 2,900 lines) again |
| **C** a private, simpler object format for cc, plus a tool run at image build time that converts clang's `libc.a` into it | our writer, our reader/linker, and a converter that reads clang's format anyway | using wasm-ld or llvm tools to check our output; linking clang objects inside the OS (only pre-converted ones) | about the same as A, plus the converter |
| **D** run clang and wasm-ld themselves, compiled to WebAssembly | nothing to write | excluded by the project rule (no third-party code); also tens of MB | |

**Chosen: A** (the brief's default, assumed on the owners' behalf; reversible because the
front end and code generator do not depend on it: `wasm.c` alone writes the format).
Reasons, as facts: it is the only option where `make` works the usual way with separate
compilation and libraries; the clang-built `libc.a`, `libui.a` and `libgfx.a` are linked
unchanged; wasm-ld and llvm-objdump could check every object cc wrote during
development; objects from the two compilers mix (tested). Cost: a link step on every
build, and the image ships the libraries and start files (342,655 bytes measured).

## 2. Structure

```
bin/cc/
  cc.h       shared declarations
  util.c     arena allocator, buffers, LEB128, hash maps, interned identifiers, diagnostics
  lex.c      characters to preprocessing tokens (whole file at once, splices removed)
  pp.c       preprocessor: Prosser hide-set expansion, #if evaluation, includes with
             guard detection and #pragma once, target predefined macros, -E output
  type.c     types, sizes and alignments of wasm32
  parse.c    C11 parser and type checker: typed tree, implicit conversions as nodes,
             constant folding, initialisers (designators, brace elision), global data
  gen.c      tree to wasm function bodies, the ABI, control flow, goto flattening
  wasm.c     the relocatable object file
  listing.c  -S: a readable listing (there is no assembler)
  main.c     the gcc-style driver
  include/   freestanding headers: stddef.h stdint.h stdarg.h stdbool.h limits.h float.h
             stdalign.h stdnoreturn.h iso646.h
  tests/     run.sh (vs clang), mix/ (mixed objects), err/ (diagnostics), fuzz.py,
             os.sh and buildall.sh (programs of the tree built by cc), bench/
bin/ld/ld.c  the linker
```

The front end (`pp.c`, `parse.c`) knows nothing about the object format; `gen.c` calls
`sym_func`, `sym_data` and friends in `wasm.c` and records relocations against symbols.

## 3. Assumptions made on the owners' behalf (reversible)

| Assumption | Alternatives | How to reverse |
|---|---|---|
| Option A (above) | B, C | replace `wasm.c` and drop `bin/ld` |
| Tool names `cc` and `ld`; no `gcc` or `c99` alias | add aliases (the kernel has no links: an alias is a copy of the program, 212 KB) | Makefile |
| Paths: libc headers `/usr/include`, compiler headers `/usr/lib/cc/include`, libraries and start files `/usr/lib` | `/usr/lib/ucos/...`, `/lib`, one directory | `main.c` (`compile_one`, link step), Makefile rule `cc-sysroot.stamp` |
| `sys/syscall.h` is installed with its include of `../../../abi/abi.h` rewritten to `<ucos/abi.h>`, abi.h at `/usr/include/ucos/abi.h` | change the libc header to `<abi.h>` with `-Iabi` in the Makefile; install abi.h at `/abi/abi.h` so the relative path works | the sed line in the Makefile rule |
| ld chooses `crt0_3.o` when `main` takes three arguments (`--crt0=A,B`, UC.OS-specific) | the driver tries one then the other as the Makefile does; one crt0 that reads main's arity some other way | `ld.c` main, `main.c` link step |
| `long double` is binary128 in memory and passed as two i64, as clang does; arithmetic calls compiler-rt helpers our libc does not have | make long double the same as double (simple, but not ABI-compatible with clang-built code such as `strtold`) | `type.c`, `gen.c` |
| Predefined `__ucos__` and `__UCOS_CC__`; `__GNUC__` and `__clang__` not defined | define `__GNUC__` so code enables GNU extensions it checks for | `pp.c` `target_macros` |
| Plain `inline` (C99 inline definition without `extern`) is emitted as a local function when used | C99 strict: emit nothing and expect an external definition elsewhere (link error at -O0 with clang) | `gen_program` |
| A call to an unprototyped function uses the promoted argument types as the wasm signature (the first call fixes it; a later different call is an error); ld checks signatures only for direct calls, not for addresses taken | always error, or a run-time trap stub as wasm-ld does without `--fatal-warnings` | `gen_call`, ld signature check |
| `-O0` to `-O3` are accepted; the same optimisations always run | real levels | `main.c` |
| The compiler makes a `getppid` system call per statement and per 64 tokens, so the kernel can time-slice a long compile | none, or a Worker | `tick()` in `util.c` |

## 4. ABI, and how it was checked

The ABI is clang's for wasm32 (tool-conventions BasicCABI.md), read from clang's own
output for probe programs (`llvm-objdump -d` of objects built with
`clang --target=wasm32-unknown-unknown`): ILP32; `long long` and `double` 8-byte aligned;
`long double` 16 bytes, binary128; `char` signed; `wchar_t` is `int`; `size_t` is
`unsigned long`, `intptr_t` is `long`. A struct holding exactly one scalar (through nested
structs and one-element arrays, no bit-fields) is passed and returned as that scalar;
unions and other structs are passed by pointer to a copy the caller makes, and returned
through a hidden first pointer argument; empty structs are not passed. Variadic
arguments go into an area of the caller's frame, each at its natural alignment
(aggregates as a pointer to a copy), and a pointer to it is the hidden last argument.
Bit-fields: allocated in units of their declared type, never crossing a unit boundary
(then moved to the next aligned unit), unnamed and zero-width ones do not change the
struct's alignment.

Verified by `bin/cc/tests/mix/`: `main.c` and `lib.c` exchange structs of every class,
bit-field structs, unions, empty structs, many mixed arguments, varargs (also `va_list`
handed to a function built by the other compiler), `long double`, function pointers
in both directions and data with relocations, linked in the three combinations (cc+clang,
clang+cc, cc+cc); output must equal the all-clang build.

## 5. Control flow without goto

WebAssembly has only structured control flow. `if`, loops and `switch` become
`block`/`loop`/`br`/`br_if`/`br_table`. A `switch` whose case labels are all at the top
level of its body becomes nested blocks entered by `br_table` (dense values: range up to
3 x the number of cases + 8, and below 4096) or by a chain of `br_if`. A function with
goto labels, or with a switch whose labels are nested in other statements (Duff's
device), is flattened: a pre-pass marks the statements that contain a jump target; only
those are cut into numbered states, placed in order inside one `loop` around a `br_table`
on a state variable; the statements without targets stay structured inside the states,
and falling from one state into the next costs nothing. A jump is `i32.const N;
local.set $state; br $dispatch`. Functions without labels pay nothing.

## 6. Measurements

Recorded with the date, the machine was shared and loaded (load average 6 to 13 on 8
threads) during all of them, so timings are indicative only.

- 2026-11-26, Node 26: cc built by cc compiles all 9 files of cc in 1.4 s user under
  `bin/cc/tests/run.mjs` (Node, wasm), about 9,500 lines plus headers.
- 2026-11-26, inside UC.OS (tools/run.mjs, Node 26, `bin/cc/tests/os/selfhost.txt`,
  load about 7): the clang-built `/bin/cc` compiles and links all of cc (9 files, one
  process per file plus ld) in 125 ms; the resulting cc2 does the same in 192 ms; cc2
  and cc3 are byte-identical (178,869 bytes). Single files: `pp.c` (1,260 lines) 72 ms,
  `parse.c` (3,100 lines) 34 ms. A cold first compile includes compiling the cc module
  itself in the engine.
- A second run of the same script at 08:40 (load about 5, final sources): 136 ms for the
  clang-built cc, 293 ms for cc2; cc2 and cc3 identical again.
- Doom (`bin/cc/tests/os/doom.sh`, doom1.wad, `-timedemo demo1`): the cc build plays the
  demo in 5026 gametics, the same as the clang build, so the game logic replayed
  identically. One run each under load 5 to 6: clang 242 realtics, cc 195 realtics. Not a
  benchmark.
- Speed of generated code: pending a quiet machine (owner rule: no benchmarks above load
  3). `bin/cc/tests/bench/run.sh` builds seven kernels (sieve, quicksort, recursion,
  strings, a float loop, a struct loop, 64-bit integer mixing) with clang -O0, clang -O2
  and cc. The only run so far was at load 13, before the optimisations, so it is noise:
  after V8 had optimised the code, cc's best batch took 0.73x to 1.44x the time of clang -O2 on
  every kernel; the cold first call (V8's baseline compiler) took 1.2x to 13x the time.
  Where the code is still weaker than clang -O2, and why: no register allocation or
  common subexpression elimination of our own (address arithmetic such as `base + i*16`
  is recomputed for each field access; V8's optimiser removes much of it, its baseline
  compiler does not); `&&`, `||` and `?:` always become `if` blocks; struct copies above
  32 bytes call `memory.copy`; no inlining.
