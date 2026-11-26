WIP notes: merging pull request #24 (rice themes, contains #21) onto main plus #9
Written 2026-11-26 about 21:05 WET, work stopped because the laptop is shutting down.

Base of this tree: main c8cb2c3 with #9 staged (now merged on main as f155264), plus #24
(head 7121a6c) applied with git apply --3way. Main has since moved to 7391d56: after #9,
pull request #33 changed boot/hostnet.mjs, docs/NETWORKING.md and docs/DECISIONS.md. Those
#33 changes are NOT in this tree; they still have to be brought in.

CONFLICTED CODE FILES (all six fully resolved, no markers left; not yet tested beyond a build)
- Makefile: resolved. UCFLAGS has both -Ilibclip and -Ilibuconf. Both archive rules kept
  (libclip.a and libuconf.a). gfx_lib = libclip (if USE_CLIP), libui + libgfx (if USE_GFX),
  then libuconf.a for every program.
- bin/term/main.c: resolved. Only the include lines conflicted: kept clip.h, uconf.h and
  errno.h. Everything else (lazy fallback font from #9, selection/clipboard and zoom from
  main, load_conf and term.uconf from #24) had merged on its own.
- bin/term/render.h: resolved. Kept main's selection fields and #9's load_fallback hook and
  #24's cursor_default field.
- bin/wm/compose.c: resolved by hand in compose_anim. Main/#9 draw the slide in bands of
  rows (strip_rows, band clip, paint_ms/put_ms); #24 added the sliding flag and a loop that
  draws sticky windows fixed on top. Written: #9's banded loop, sliding = 1 before it and
  0 after, and #24's sticky loop inside each band using &band instead of &all.
- bin/wm/config.c: resolved. #24's built-in binding table plus {"Mod+y", "float"} from #9.
  Every built-in of main is present (Mod+f fullscreen, Mod+Shift+space float, Mod+y float,
  Mod+Left/Right focus, zoom keys, workspaces 1..9).
- bin/wm/tests/run.sh: resolved. List: layout workspace slide zoom split float robust misc
  pointerlock perf showcase.

DOCUMENT CONFLICTS (left with markers on purpose, for the documentation session):
docs/ARCHITECTURE.md, docs/DECISIONS.md, docs/DEVELOPMENT.md, docs/STATUS.md.

CHANGES BEYOND THE CONFLICTED FILES
- rootfs/home/user/.config/wm/config: removed (from the index with git rm --cached and from
  the disk), as #24 does.
- rootfs/home/user/.config/wm/wm.uconf: #9's float binding carried over, after
  "bind = $mod, f, fullscreen":
    # float: the window leaves the tiling and the others fill its place.
    # Hold $mod and drag a floating window with the left button to move it.
    bind = $mod SHIFT, space, float
    bind = $mod, y, float
  (the Mod+Shift+space line was already there; the comment and Mod+y are new)
- bin/blueprint/term.h: replaced by #24's bin/vi/term.h (byte-identical copies before #24;
  checked: main's blueprint/term.h == main's vi/term.h). Blueprint builds; not run yet.
- bin/doom/tests/mkimage.sh: with WM=1 it now writes
  "[general] gaps = 0 [autostart] exec = doom" into build root .config/wm/wm.uconf instead
  of the old config file; still removes a stale old config file. Not run yet.
- bin/wm/main.c: one guard added to #9's Mod+drag start: "&& !c->nofocus", so the cheat
  sheet (a sticky, nofocus floating overlay from #24) is never picked up, because picking a
  window up calls set_focus and would give the overlay the keyboard focus. Mechanical choice;
  to reverse, delete "&& !c->nofocus" and the comment line above it.
- grep for the old ~/.config/wm/config found no other code or test that reads or writes it
  (bin/wm/tests, bin/term/tests, tools/tests are clean). Only mkimage.sh needed moving.

BUILDS AND TESTS RUN
- nice -n 19 make B=build-c24 WADS=/home/ensui/uc-os/bin/doom/wads: exit 0, no warnings,
  no errors in the log.
- Sizes: initrd.tar 11,141,120 bytes (reference main+#9, c9q/build-vfy: 10,823,680, so
  +317,440); kernel.wasm 80,774 (same). build-c24/bin has 105 entries against 102 in the
  reference directory: the three new ones are keys, themes, uconftest.
- make test: started, then stopped by the shutdown request before it printed anything.
  NO TEST SUITE HAS BEEN RUN. Nothing is known to work beyond the build.

STILL TO DO, IN ORDER
1. Bring the tree up to main 7391d56 (#33: boot/hostnet.mjs, docs/NETWORKING.md,
   docs/DECISIONS.md) and rebuild on a fresh build directory.
2. make test (kernel 124, C library 298, uconf, shell 238, sound 37, network 89, router 24).
3. Window manager scripts, each with PASS/FAIL counts: layout workspace slide zoom split
   float robust misc pointerlock perf showcase (helper: build-c24/tmp/wmall.sh, gate in
   build-c24/tmp/gate.sh). Watch float, pointerlock, zoom, slide under the new layouts and
   the cheat sheet that now autostarts (lib.mjs boot() hides it with Alt+/; zoom.mjs does
   not call boot(), check it).
4. Terminal suites bin/term/tests/os/ (basic, apps, theme, clipboard with --init=/bin/term;
   window.mjs with --init=/bin/wm --size=1280x720), bin/clipboard/shots.mjs,
   tools/tests/dotfiles.mjs, tools/tests/desktop.mjs (look at screenshots), blueprint
   navigation test and bin/blueprint/shots.mjs, Doom launcher test
   (--remote-rate=2000000 bin/doom/tests/launcher.mjs) and WM=1 mkimage window test.
5. Theme picker and three or more themes: screenshots of term, bar, launcher, fetch, vi,
   htop, sysbench, blueprint in each.
6. Report.

REVIEW NOTES FOR #24'S AUTHOR (not fixed, their code)
- A sticky window is drawn on both halves during the workspace slide (reported by review;
  not reproduced yet).
- libuconf: on out-of-memory the parser stores a NULL name.
- libuconf/uconf.c around line 178: keys longer than 63 characters are cut silently
  (char key[64]).

DOCUMENT MENTIONS OF THE OLD CONFIG (for the documentation session; not edited)
- docs/ARCHITECTURE.md 544-547 (ours side of a conflict): ~/.config/wm/config, i3 style,
  set mod Alt, border_thin, workspace_count, animation_ms, bind Mod+KEY.
- docs/ARCHITECTURE.md 531: animation_ms (wm.uconf calls it general.animation).
- docs/STATUS.md 23 and 27: animation_ms.
- docs/SLIDES-REWRITE.md 449: /usr/share/themes/*.theme (themes are .uconf now).
- docs/DECISIONS.md 46: mentions the old file only to say it is gone (fine).
