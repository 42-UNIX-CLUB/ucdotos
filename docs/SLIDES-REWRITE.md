# Rewriting `slides` in C: analysis

Written 2026-11-26. Status: analysis. Section 9 lists the choices that belong to the owners.

Update, same day: an owner asked for a minimal version for the files in `~/unix_club`
only. It is `blueprint` (`bin/blueprint/`) and it takes none of the larger options below: no Markdown
rendering, no front matter, search, Ctrl+E, clipboard, pre-processing or SSH. What it
does and what it assumed is in `DECISIONS.md` section 2 and `STATUS.md`. The rest of
this file still applies to any fuller version.

Source analysed: https://github.com/maaslalani/slides, commit `92af1ef` (2026-07-08,
"bubbletea v2"), MIT licence. Its libraries were read at the versions in its `go.mod`:
glamour v1.0.0, goldmark v1.7.13, chroma v2.20.0.

How this was checked. Every Go file of slides was read in full. The glamour files that
decide what a slide looks like were read in full (`ansi/*.go`, `glamour.go`). goldmark and
chroma were only sized and skimmed. Nothing from the original was compiled or run (an
attempt to run its tests on edge cases was blocked in the session that wrote this). Where
this file says "read from the code, not run", the behaviour is what the source says, not
what was observed.

## 1. What slides is

A terminal program that shows a Markdown file as a slideshow. One slide fills the
terminal. A status bar at the bottom shows author, date and "Slide N / M". Keys move
between slides, search them, and run the code blocks on the current slide.

Size, measured on the clone (lines of `.go`, tests excluded unless stated):

| Part | Lines | Role |
|---|---|---|
| slides itself | 2,279 (849 of them tests) | loading, splitting, front matter, keys, search, code, pre-processing, SSH |
| glamour | 4,097 | Markdown to ANSI text, the look of every element |
| goldmark | 13,430 | the Markdown parser glamour uses (CommonMark + GFM) |
| chroma | 10,618, plus 257 XML lexer files | syntax highlighting of code blocks |
| Bubble Tea, Bubbles, Lip Gloss, wish, termenv, yaml.v2 | not measured | event loop, text input, layout, SSH server, terminal detection, YAML |

So slides' own logic is small. Most of what you see on screen comes from glamour,
goldmark and chroma. A C rewrite is mostly a rewrite of those three, cut down to what
slides uses.

## 2. Architecture of the original

```
main.go           parse argv (coral), build Model, Model.Load(), tea.NewProgram(model).Run()
  model.Load      read file or stdin -> remove \r -> pre-process if executable
                  -> split on "\n---\n" -> parse front matter from chunk 0 -> pick theme
  Bubble Tea      calls Update(msg) on every key, window size and 1 s timer tick,
                  then View() to redraw the whole screen
  model.Update    search mode keys, then "/", ctrl+n, ctrl+e, y, q, else navigation.Navigate
  model.View      glamour renders the current slide's Markdown at terminal width,
                  appends "virtual text" (code output), adds padding, joins status bar
```

Internal packages and their public functions:

| Package | File | API | Notes |
|---|---|---|---|
| `model` | `model.go` | `Model{Slides, Page, Author, Date, Theme, Paging, FileName, VirtualText, Search}`, `Load()`, `Init()`, `Update()`, `View()`, `SetPage()`, `CurrentPage()`, `Pages()` | the whole program state |
| `meta` | `meta.go` | `New()`, `(*Meta).Parse(header) (*Meta, bool)` | front matter, defaults, date format |
| `navigation` | `navigation.go` | `State{Buffer, Page, TotalSlides}`, `Navigate(State, key) State` | pure function, vim-style counts |
| `navigation` | `search.go` | `Search{Active, SearchTextInput}`, `Begin()`, `Done()`, `Query()`, `SetQuery()`, `Execute(Model)` | regex search over raw slide text |
| `code` | `code.go`, `comments.go`, `languages.go` | `Parse(md) ([]Block, error)`, `Execute(Block) Result`, `HideComments`, `RemoveComments`, `Languages` map | ctrl+e |
| `process` | `process.go` | `Parse(md) []Block`, `(*Block).Execute()`, `Pre(content) string` | `~~~cmd` blocks |
| `file` | `file.go` | `Exists`, `IsExecutable` | |
| `styles` | `styles.go`, `theme.json` | `Author Date Page Slide Status Search` styles, `JoinHorizontal`, `JoinVertical`, `SelectTheme(name)` | status bar look, theme choice |
| `cmd`, `server` | `serve.go`, `server.go`, `middleware.go` | `ServeCmd`, `NewServer`, `Start`, `Shutdown` | SSH server |

## 3. Dependencies and what slides actually uses from each

| Library | Used for | What slides really needs from it |
|---|---|---|
| Bubble Tea v2 | event loop, alternate screen, key decoding, window size | read keys in raw mode, redraw on change, a 1 s timer, resize |
| Bubbles v2 | `textinput` (search bar), `viewport` (only to store width and height) | a one-line input with prompt `/`, placeholder "search", Enter and Esc |
| Lip Gloss v2 | status bar styles, padding, `JoinHorizontal`/`JoinVertical`, glamour's tables | padding, left/right placement, fill to height, table layout |
| glamour | Markdown to ANSI | everything in section 4.6 |
| goldmark (via glamour) | Markdown parser: CommonMark + GFM (tables, strikethrough, linkify, task lists) + definition lists | a block and inline parser for those elements |
| chroma (via glamour) | highlighting fenced code, theme "dracula", formatter "terminal256" | a lexer per language and a colour per token type |
| bluemonday (via glamour) | strips all HTML tags (`StrictPolicy`) | remove `<...>` tags, keep their text |
| reflow, x/ansi (via glamour) | word wrap that ignores escape codes | wrap at a width, break at spaces and ` ,.;-+|` |
| termenv | `NO_COLOR`, dark or light background detection, colour output | pick the theme; write SGR codes |
| yaml.v2 | front matter | four string keys |
| coral | command line (`slides FILE`, `slides serve FILE`) | argv |
| wish, charmbracelet/ssh, keygen | `slides serve` | SSH server |
| atotto/clipboard | `y` copies code | system clipboard |
| testify | tests | |

## 4. Every feature and its exact behaviour

### 4.1 Input

- `slides FILE` reads FILE. A directory or missing file is an error ("could not read
  file", "can not read directory").
- `slides` with no file reads stdin. If stdin is not a pipe and its size is 0, it shows a
  built-in tutorial (`internal/model/tutorial.md`, 77 lines, embedded in the binary).
- All `\r` are removed.

### 4.2 Splitting into slides

- If the text starts with `---\n`, that first `---\n` is removed.
- The text is split on the exact string `\n---\n`. A line `--- ` (trailing space) or
  `----` is not a separator. A `---` inside a fenced code block still splits (it is plain
  string splitting, Markdown is not parsed here).
- Chunk 0 is tried as front matter (4.3). If it parses and there is more than one chunk,
  chunk 0 is dropped.

### 4.3 Front matter

Keys (all strings, any other key is ignored): `theme`, `author`, `date`, `paging`.
Parsed with yaml.v2 into a struct of string pointers, so "key missing" and `key: ""` are
different.

| Key | Missing | Present |
|---|---|---|
| `theme` | `$GLAMOUR_STYLE`, else `default` | the value |
| `author` | the OS user's full name (`user.Current().Name`), `""` on error | the value; `""` hides it |
| `date` | today as `YYYY-MM-DD` | tokens replaced in this order: `YYYY`, `YY`, `MMMM`, `MMM`, `MM`, `mm`, `DD`, `dd` (2006, 06, January, Jan, 01, 1, 02, 2). If no token was found, the text is shown as is. Otherwise today's date is formatted with it. |
| `paging` | `Slide %d / %d` | 2 `%d`: page and total; 1 `%d`: page; 0 or 3 or more: the text as is |

If chunk 0 is not valid YAML for that struct (for example a heading followed by a line
of text), the defaults are used and chunk 0 stays a slide.

### 4.4 Themes

`SelectTheme(name)`:
- `ascii`, `light`, `dark`, `notty`: glamour's built-in style of that name.
- anything else, including `default`: if it starts with `http`, fetch it over HTTP;
  otherwise open it as a file path. The content is a glamour JSON style (the format in
  `styles/theme.json`, 103 lines).
- if that fails: `notty` when `NO_COLOR` is set, glamour's `light` when the terminal
  background is light, otherwise slides' own `theme.json` (the default look).
- Note: `default` is not a special name. It is tried as a file called `default` first.
- The theme is chosen once. A live reload (4.10) does not change it.

The glamour JSON style format: per element (`document`, `heading`, `h1`..`h6`,
`paragraph`, `block_quote`, `list`, `item`, `enumeration`, `task`, `text`, `emph`,
`strong`, `strikethrough`, `hr`, `link`, `link_text`, `image`, `image_text`, `code`,
`code_block`, `table`, `definition_*`, `html_*`) these fields: `block_prefix`,
`block_suffix`, `prefix`, `suffix`, `color`, `background_color` (an ANSI 256 index like
`"252"` or hex like `"#9fc"`), `bold`, `italic`, `underline`, `crossed_out`, `faint`,
`inverse`, `blink`, `conceal`, `overlined`, `upper`, `lower`, `title`, `format` (a Go
template, `{{.text}}`), and for blocks `margin`, `indent`, `indent_token`; `list` has
`level_indent`; `task` has `ticked`/`unticked`; `table` has the three separators;
`code_block` has `theme` (a chroma style name) or `chroma` (a colour per token type).
Styles cascade from parent block to child: colours and attributes are inherited, a child
value overrides.

### 4.5 Screen layout

- Alternate screen on.
- The slide is rendered with word wrap at the terminal width, then given 1 cell of
  padding on every side (Lip Gloss `Padding(1)`). glamour's `document` style adds a
  margin of 2 and a blank line before and after.
- Tabs in the rendered slide become 4 spaces.
- The status bar is at the bottom, with 1 cell of padding (so it takes 3 rows). Left:
  author in `#E8B4BC` with a left margin of 2, then the date, faint, with a margin of 1.
  Right: paging text in `#E8B4BC` with a right margin of 3. While searching, the left part
  is replaced by the search input (faint).
- The slide is placed at the top and the status bar at the bottom of the terminal height.
  What happens when a slide is taller than the screen was not checked (Lip Gloss does not
  cut it; the Bubble Tea v2 renderer was not read).

### 4.6 Markdown rendering (glamour with slides' default theme)

| Element | How it looks |
|---|---|
| `# h1` | `██ Title ` bold, `#9fc`, blank line after (all headings: bold, colour 39 unless the level overrides, `block_suffix` newline) |
| `## h2` .. `#### h4` | prefixes `▓▓▓ `, `▒▒▒▒ `, `░░░░░ `; colours `#1cc`, `#29c`, `#559` |
| `#####`, `######` | heading defaults, no prefix |
| paragraph | word-wrapped to the width left after margins and indents |
| `*em*`, `**strong**`, `~~strike~~` | italic, bold, crossed out |
| `` `code` `` | ` code ` with a space each side, colour 203 on 236 |
| fenced or indented code | highlighted by chroma with style `dracula`, 256 colours, margin 2. Not wrapped. |
| `> quote` | each line starts with `│ `, indent 1 |
| `* item` | `• item`; nested lists indent 2 per level |
| `1. item` | `1. item`, numbering follows the list's start number |
| `- [x] task` | `[✓] task` / `[ ] task` |
| `[text](url)` | text in colour 35 bold, a space, url in colour 30 underlined. A URL that is only `#anchor` is not shown. |
| bare URLs (linkify) | the URL, styled as a link |
| `![alt](src)` | `Image: alt →` in colour 243, then src in colour 212 underlined |
| table | Lip Gloss table: `│` between columns, `─` and `┼` under the header, no outer border, column alignment from the `:---:` row, cells wrapped to fit the width. Links inside cells become numbered footnotes under the table. |
| `---` (thematic break inside a slide) | cannot happen: the splitter already consumed `\n---\n` |
| `***` or `___` | `--------` in colour 240 |
| definition list | term, then `🠶 description` |
| HTML | tags removed, text kept |
| emoji `:smile:` | not converted (glamour's emoji option is off) |
| footnotes `[^1]` | not enabled |

Also: `\*`, `\_` and the other Markdown escapes are shown without the backslash; HTML
entities are decoded.

### 4.7 Keys and navigation

Handled before navigation, in this order:

| Key | Action |
|---|---|
| (search active) Enter | if the query is not empty, run the search; else close the search bar |
| (search active) Esc, Ctrl+C | clear the query, close the search bar |
| (search active) other | edited by the text input |
| `/` | open the search bar with an empty query |
| Ctrl+N | run the last search again |
| Ctrl+E | run the code blocks of this slide (4.9) |
| `y` | copy the code blocks to the clipboard |
| `q`, Ctrl+C | quit |

Then `Navigate(state, key)`. `buffer` holds a pending count or a pending `g`:

| Key | Result |
|---|---|
| `0`..`9` | append to the buffer if it is a number, else start a new number. Page unchanged. |
| `g` | if the buffer is `g`: go to slide 1. Else the buffer becomes `g`. |
| `G` | with a number N in the buffer: go to slide N (clamped to 1..last). Without: last slide. |
| Space, Down, `j`, Right, `l`, Enter, `n`, PgDn | forward 1, or N times with a count (0 counts as 1). Stops at the last slide. |
| Up, `k`, Left, `h`, `p`, PgUp, `N` | back 1 or N. Stops at the first slide. |
| any other key | clears the buffer |

Every navigation key clears the buffer. Changing page clears the virtual text. The
original's tests (14 cases in `navigation_test.go`) pin this: for example `0j` gives 1,
`101G` gives the last slide, `-11G` gives the last slide (the `-` clears the buffer).

### 4.8 Search

- The query is a Go RE2 regular expression. A trailing `/i` makes it case-insensitive.
- An invalid expression does nothing.
- It searches the raw Markdown of each slide (not the rendered text), starting at the
  next slide, wrapping to the first, stopping before the current one. The current slide
  is never a match. The first slide that matches becomes the page.

### 4.9 Code execution (Ctrl+E), comments, yank

- Blocks are found with the regex ``(?s)(?:```|~~~)(\w+)\n(.*?)\n(?:```|~~~)\s?``. So a
  block needs a language word right after the fence; a block without one is skipped.
  Opening and closing fences may differ (```` ``` ```` and `~~~` are interchangeable).
- No block at all: the virtual text becomes `Error: could not parse code block`.
- For each block: look up the language in a table of 20 (`bash zsh fish elixir go
  javascript lua ruby ocaml python perl rust java julia cpp swift dart v scala haskell`).
  Unknown: `Error: unsupported language`. Else write the code to a temp file
  `slides-*.EXT` and run the language's commands (for example `bash <file>`, or
  `rustc <file> -o <path>/<name>.run` then `<path>/<name>.run`). Only stdout is kept. If a
  command fails, the Go error text (for example `exit status 1`) is shown instead.
- The outputs of all blocks are joined with newlines and shown under the slide, unstyled,
  until the page changes.
- Comments: a line starting with `///` is hidden on screen (`HideComments`). When the
  code runs or is copied, `///` is removed and the rest of the line is kept
  (`RemoveComments`). This lets a slide show only part of a program and still run all of
  it.
- `y` copies each block in turn to the system clipboard (so the last block wins).

### 4.10 Pre-processing and live reload

- Pre-processing runs only if the file has all three execute bits (`0111`). A first line
  starting with `#!` is removed first.
- Each block `~~~COMMAND\nINPUT\n~~~` runs COMMAND with INPUT on stdin, and the block is
  replaced with the command's stdout. COMMAND is split on single spaces with no quoting,
  so `~~~echo "a b"` passes `"a` and `b"` as two arguments (read from the code, not run).
  If the command fails, the block is replaced with nothing.
- Read from the code, not run: a normal Markdown code block fenced with `~~~bash` would
  also be treated as a pre-process command (`bash` with the code as stdin) when the file
  is executable.
- Live reload: when a file name was given, every second the file's modification time is
  compared with the last one. If it changed, the file is loaded again (including
  pre-processing) and the page is clamped to the new last slide. Stdin input is not
  watched.

### 4.11 SSH server (`slides serve FILE`)

Starts an SSH server (default `localhost:53531`, host key file `slides`; flags `--host`,
`--port`, `--keyPath`; env `SLIDES_SERVER_HOST`, `SLIDES_SERVER_PORT`,
`SLIDES_SERVER_KEY_PATH`). Each SSH client with a pty gets its own copy of the
presentation. Clients without a pty are refused.

### 4.12 Behaviour a faithful port must decide to copy or not

These come from the code. The ones marked (not run) were not confirmed by running it.

1. A first slide that is only a YAML comment, for example a lone `# Title` line, parses
   as empty valid YAML, so it would be taken as front matter and dropped when there are
   other slides (not run).
2. `---` inside a code block splits the slide.
3. `~~~lang` code fences run as commands in executable files (not run).
4. Pre-process commands are split on spaces without quotes (not run).
5. `theme: default` first looks for a file called `default`.
6. The theme does not change on live reload.
7. `y` keeps only the last block.
8. Search never matches the current slide.

## 5. What UC.OS already has that a rewrite can use

Checked in this tree on 2026-11-26.

| Need | In UC.OS | Where |
|---|---|---|
| raw mode, window size, key reading with timeout, UTF-8 helpers, truecolor output buffer, deck/brand theme following Alt+t | yes, as a header copied into each program | `bin/vi/term.h` (same file in `bin/htop/`, `bin/fetch/`) |
| resize | no signals; programs poll `tty_size` | `bin/htop/htop.c:446` |
| `poll`, `select`, `nanosleep`, `clock_gettime` | yes | `libc/include/` |
| `stat` with `st_mode` and `st_mtime` | yes. `st_mtime` is in whole seconds. File modes come from the image's tar. | `abi/abi.h:130`, `kernel/vfs.c:172` |
| `chmod` | no system call, no program | |
| `popen`, `pipe2`, `spawn`, `waitpid`, `dup2` | yes. `popen` runs `/bin/sh -c`. | `libc/src/stdio.c:195` |
| `strftime`, `localtime_r` | yes. Time is UTC (no time zones). | `libc/src/time.c` |
| regular expressions | no `regex.h`. vi has a 94-line matcher: literals, `^ $ . * [...]`, `\` escapes | `bin/vi/regex.c` |
| JSON, YAML parsers | no | |
| user names | no users. `USER=user` in the environment. | `bin/init.c` |
| terminal: SGR bold, faint, italic, underline, blink, inverse, hidden, strike, 256 and 24-bit colour, alternate screen 1049, mouse, bracketed paste | yes. Italic is drawn upright. | `bin/term/vt.c:365`, `docs/STATUS.md` |
| terminal: overline (SGR 53), OSC 52 clipboard, OSC 8 links, OSC 11 background query, sixel or kitty images, double-width characters | no | `bin/term/vt.c` |
| glyphs of the default theme `█ ▓ ▒ ░ • ✓ → │ ┼ ─` | present in Fira Code and JetBrains Mono | measured with a cmap reader |
| glyph `🠶` (U+1F836, definition lists) | missing in both fonts | same |
| clipboard, network, SSH, other languages (go, python, node, rustc ...) | no | `docs/STATUS.md` section 3 |
| club content that could become a deck | `~/unix_club/*/*.md` and `intro.sh`, which already pages through them one file at a time | `rootfs/home/user/unix_club/` |

## 6. Feature to C mapping

Proposed modules are a sketch for discussion, under `bin/blueprint/` if a terminal program
(the form is an owner decision, section 9).

| Original feature | Proposed C | Uses from UC.OS | Works in UC.OS |
|---|---|---|---|
| argv, read file, read stdin, tutorial | `main.c`: `open/read`, `fstat` to tell a pipe; tutorial as a C string or a file in `/usr/share` | libc | yes |
| `\r` removal, `\n---\n` split, front matter | `deck.c`: split in place into an array of `char *`; a line parser for `key: value` with optional quotes | libc | yes |
| date tokens | `deck.c`: token replace, then `strftime` | `strftime` | yes, UTC |
| author default | `getenv("USER")` or empty | env | partly: there is no full name |
| paging | count `%d`, `snprintf` | libc | yes |
| Bubble Tea loop | `main.c`: raw mode, `poll` on stdin with a 1000 ms timeout, redraw when state changes, `tty_size` each wake | `term.h` | yes |
| key decoding (arrows, PgUp/PgDn, Ctrl+N, Ctrl+E) | `key_read` from `term.h` | `term.h` | yes |
| navigation state machine | `nav.c`: a pure function, port of `Navigate`, with the 14 original test cases | none | yes |
| search input | `main.c`: one-line editor in the status bar (`/`, Enter, Esc, Backspace) | | yes |
| search matching | `search.c`: one of the options in 9.4 | maybe `bin/vi/regex.c` | yes, syntax depends on the option |
| Markdown parser | `md.c`: blocks (headings ATX and setext, paragraphs, fenced and indented code, quotes, lists with nesting and tasks, tables, thematic breaks, HTML blocks, definition lists) then inlines (emphasis, strong, strike, code spans, links, autolinks, images, escapes, entities, raw HTML) | none | yes |
| renderer and word wrap | `render.c`: walk the tree, keep a style stack (cascade as in glamour), wrap by display width at spaces and ` ,.;-+|`, emit SGR | `term.h` UTF-8 helpers | yes |
| tables | `render.c`: measure columns, shrink to width, wrap cells, separators, alignment | | yes |
| themes | `style.c`: options in 9.3 | maybe `theme_reload` from `term.h` | yes |
| syntax highlighting | `hl.c`: options in 9.5 | | yes, coverage depends on the option |
| status bar, padding, layout | `main.c` | `term.h` | yes |
| `///` comments | `code.c`: hide on display, strip on run | | yes |
| Ctrl+E | `code.c`: options in 9.6 | `spawn`, pipes, `waitpid` | partly |
| `y` | options in 9.7 | | no system clipboard today |
| pre-processing | `pre.c`: find blocks, `spawn` the command with pipes for stdin and stdout, replace | `spawn`, pipes | partly: needs the execute bit and the command to exist |
| live reload | `main.c`: `stat` on every 1 s wake, compare `st_mtime` | `stat` | yes, with 1 s granularity (two saves in the same second look like one) |
| remote theme over HTTP | none | | no network |
| SSH server | none | | no network |

## 7. Features that cannot work directly in UC.OS, and alternatives

Each item lists options. None is preferred here.

**SSH server (`slides serve`).** Needs TCP. A browser page cannot open sockets, and UC.OS
has no networking (open decision in `DECISIONS.md` section 3).
- Leave it out.
- Leave it out now; revisit if networking is built (a relay server would be needed).
- Replace with something local, for example presenting in one window while another
  terminal shows the same deck (no network involved; needs its own design).

**Remote themes (`theme: https://...`).** Needs HTTP. Same open decision.
- Accept only file paths.
- Keep the code path and fail to the default, as the original does when a fetch fails.

**System clipboard (`y`).** UC.OS has no clipboard (`STATUS.md`: "no clipboard").
- Leave `y` out.
- Write the code to a file (for example `/tmp/slides-yank`) so it can be used from the
  shell.
- Emit OSC 52 for when the terminal gains clipboard support (it does nothing today).
- Build a clipboard in the desktop first (a separate piece of work).

**Running code in 20 languages (Ctrl+E).** None of those interpreters or compilers exist
in UC.OS. What does exist: `sh`, `bc`, and every program in `/bin`.
- Support only what exists: `sh`/`bash` blocks run with `/bin/sh`, `bc` blocks with `bc`.
- Keep the original's table so the same deck behaves the same elsewhere; unsupported
  languages show `Error: unsupported language` or a "not installed" message.
- Make the table a config file so new languages can be added without rebuilding.

**Pre-processing with outside tools (`graph-easy`, `plantuml`, `cat`).** Only UC.OS
programs can run.
- Keep the feature as is; it works for UC.OS commands (`cat`, `sh`, `sort`, `seq` ...).
- Leave it out.

**Execute bit as the safety switch.** UC.OS has no `chmod`. A file's mode comes from the
image (`rootfs/` permissions in the tar). Files made inside the running system get the
mode they were created with.
- Keep the rule; decks shipped in the image can be marked executable at build time.
- Add a command-line flag instead of (or as well as) the execute bit.
- Add `chmod` to the kernel and tools (a separate piece of work).

**Dark or light background detection (`termenv.HasDarkBackground`).** Uses the OSC 11
query, which `term` does not answer.
- Use the UC.OS theme (`deck` or `brand`) to choose, as vi and htop do.
- Always assume dark.

**Author default (OS full name).** UC.OS has no users.
- Use `$USER` (`user`).
- Default to empty.

**Images.** The original never draws images either, it prints `Image: alt → url`. No
browser limit is involved. A graphical version could draw them (9.1).

**Definition list arrow `🠶`.** Not in either font; it would be drawn by the fallback or
as a missing glyph. Replace with a glyph the fonts have (for example `→`, present in
both), or keep it.

**Italic.** `term` draws italic upright. `*emphasis*` will look like plain text until
the terminal draws italic.

## 8. Rough size of a C rewrite

These are estimates, not measurements. For comparison: `bin/vi/vi.c` is 1,667 lines,
`bin/htop/htop.c` is 506.

| Module | Estimate (lines) |
|---|---|
| `main.c` loop, status bar, search input, reload | 300 to 450 |
| `deck.c` split, front matter, date, paging | 150 to 250 |
| `nav.c` + `search.c` | 100 to 250 (more if a full regex engine is written) |
| `md.c` Markdown parser for the elements in 4.6 | 900 to 1,500 |
| `render.c` styles, wrap, tables | 700 to 1,100 |
| `style.c` | 100 (built-in only) to 500 (with a JSON parser) |
| `hl.c` | 0 (none) to 600+ (several hand-written lexers) |
| `code.c` + `pre.c` | 200 to 300 |
| total | about 2,500 to 5,000 |

Tests worth porting: `navigation_test.go` (14 cases), `meta_test.go` (date and default
cases), `comments_test.go`, `code_test.go`, `process_test.go`, `search_test.go`. They can
run natively like `bin/sh/test/le_test.c`, plus a `shots.mjs` desktop script like
`bin/vi/shots.mjs`.

## 9. Decisions for the owners

Nothing below is decided. Each needs a choice before the matching code is written.

**9.0 Third-party rule and licence.** CLAUDE.md rule 3 says no third-party code unless an
owner asks. A rewrite that follows slides' code and copies its tutorial, theme and
behaviour is probably a derivative of MIT-licensed work.
- Port from the source: keep the MIT notice and credit for the derived parts (MIT can be
  combined with GPL-2.0-or-later), and record the exception in `DECISIONS.md` like Doom.
- Write it from this behaviour description only, with our own text and theme, and credit
  slides as the inspiration.
- Something in between (port the logic, write our own tutorial and theme).

**9.1 Form.**
- A terminal program (`term -e slides FILE`), like the original. Reuses `term.h`.
- A graphical libui window. Could draw images, large headings and real italic, but no
  longer the same program as the original.
- Terminal first, graphical later.

**9.2 Name.** Decided 2026-11-26: `blueprint` (DECISIONS.md, 18).

**9.3 Themes.**
- Built-in only: one look derived from `deck` and one from `brand`, switching with Alt+t
  like the other programs.
- Also read glamour JSON style files (needs a JSON parser, about 300 to 400 lines).
- Also or instead, a `key = value` file in the style of `/usr/share/themes/*.theme`.
- What `theme: dark|light|ascii|notty` mean: map to the above, or ignore.

**9.4 Search syntax.**
- Literal text only, with `/i` for case-insensitive.
- vi's matcher (`^ $ . * [...]`), moved to a shared place.
- A larger regex engine (alternation, groups, `+ ?`, classes) close to RE2.

**9.5 Syntax highlighting.**
- None: code blocks in one colour.
- A few hand-written lexers (for example `c`, `sh`, `md`, and whatever the club decks use).
- A generic lexer: keywords, strings, comments and numbers from a small per-language
  table.
- A port of chroma's XML lexer format (needs a full regex engine; 257 lexer files exist).

**9.6 Ctrl+E languages.** See section 7.

**9.7 `y` (clipboard).** See section 7.

**9.8 Pre-processing.** Keep, drop, or change the safety switch. See section 7.

**9.9 The original's quirks (4.12).** Copy them for compatibility, or fix them.

**9.10 Scope of the first version.** Which of these ship first: splitting and front
matter, navigation, rendering, search, live reload, Ctrl+E, pre-processing, themes,
highlighting.

**9.11 Content.** Whether the club's `~/unix_club` pages become a deck file shown with
the new program, and whether `intro.sh` changes to use it.

## 10. Not verified

- None of the original was run. Items marked "(not run)" come from reading the code.
- Bubble Tea v2's behaviour with slides taller than the screen.
- glamour's exact spacing between consecutive blocks was read, not compared against real
  output. A port that must match the original's look line for line should first capture
  the original's output for a set of test decks on a normal Linux machine.
- The line estimates in section 8.
