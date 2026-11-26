# Brand, look and voice

What UC.OS should look and read like, and the rules that come with the club's identity.
The source is the club's own "Master Brief" and its intro deck. Those documents are in
the owners' working tree but not in git (they contain personal contact details), so the
facts an agent needs are copied here.

## 1. The club

- **42 UNIX Club** (short: 42 UNIX), a student club at 42 Lisboa (part of the 42 Network)
  for the UNIX philosophy, customization, and ricing. Founded in July 2026 by two
  students; intra usernames rucosta and mju-ferr. Three people work on UC.OS.
- Personality in three words: **Minimal. Deliberate. Open.**
  - Minimal: small tools that do one thing well; lean documents; tables over paragraphs.
  - Deliberate: every choice has a reason and states it.
  - Open: free and open-source software, hardware and assets; and open to newcomers,
    in plain language, nothing gatekept.
- Three pillars: educational (demystify UNIX and the terminal), execution and showcasing
  (ricing, dotfiles, members' setups), peer support (a resource to learn from, not a
  help desk and never a source of project solutions; 42's academic-honesty norms apply).
- "Ricing" means making your system look and work exactly how you want: colours, fonts,
  window manager, bar, terminal, prompt. UC.OS is itself a rice, and its dotfiles are
  teaching material: keep them heavily commented.

## 2. Name

**UC.OS**, said "UC dot OS", standing for UNIX CLUB dot OS. Write it exactly `UC.OS` in
text. The repository is `ucdotos`. `uname` reports `UC.OS`. Never call it DuxOS (an
abandoned working name).

## 3. The two themes

Both ship. The user switches with Alt+t or by editing `~/.config/ucos/theme`.

### `deck` (default): the look of the club's intro deck

Gruvbox dark with Fira Code. Slide 26 of the deck ("Anatomy of a rice") is the target
layout: a thin top bar (workspace numbers left with the current one in an orange block,
date and time centred, status right), tiled windows with rounded corners and gaps, a
2 px orange border on the focused window, a launcher popup with a yellow border and a
`›` prompt.

| Role | Hex |
|---|---|
| background | `282828` |
| foreground | `ebdbb2` |
| orange (accent, focus border) | `fe8019` |
| red | `fb4934` |
| yellow (launcher border) | `fabd2f` |
| green | `b8bb26` |
| aqua | `8ec07c` |
| blue | `83a598` |
| purple | `d3869b` |
| grey (dim text) | `928374` |
| panels / bar | `3c3836` |
| desktop behind the windows | `1d2021` |

### `brand`: the club's final brand palette

Dark green with JetBrains Mono. Hex values are the club's locked digital source of
truth. Do not adjust them.

| Role | Hex |
|---|---|
| terminal dark (background) | `0E362E` |
| cream (text, primary marks) | `EDE6C5` |
| sage (the chevron) | `ADC787` |
| orange (cursor, accent) | `FE7700` |
| red (dot) | `FC413A` |
| yellow (dot) | `FFC72E` |
| green-yellow (dot) | `A9B200` |

The brand defines only these seven. A terminal needs sixteen colours, so the theme file
contains derived blue, purple and aqua values that are marked as **placeholders the club
has not approved**. Do not present them as brand colours.

### Fonts

- **JetBrains Mono**: the club's hero typeface (brand theme). The image ships the Nerd
  Font build, Regular and Bold.
- **Fira Code**: the deck's typeface (deck theme), Regular and Bold.
- Both are under the SIL Open Font License; the licence texts ship in the image.
- **Inter** is the club's face for long body text in documents. It is not used in the OS.

## 4. The logo

A terminal-prompt mark: three window-control dots (red, yellow, green-yellow), a sage
chevron `❯`, the numerals **42**, and an orange cursor bar. Read as `❯42|`.

- The 42 numerals are the official 42 logo glyphs. The chevron, cursor and dots are the
  club's own work.
- In UC.OS the mark appears in `fetch` (extruded and rotating in 3D), dimmed on the
  window manager's wallpaper, and in the editor's statusline.
- The shell prompt and the mark share a shape on purpose: "the identity is the terminal".

### Rules that come with the 42 glyphs (from the club's brief)

- 42 UNIX is a student club **at** 42 Lisboa, not an official 42 product. Never imply
  endorsement.
- Never recolor the 42 logo into the club palette outside the club mark itself, never
  merge or alter it.
- Public use of the 42 logo should be cleared with the campus first. A public UC.OS site
  that shows the mark falls under this. It is an open item for the owners
  (DECISIONS.md section 3).
- The GPL licence of UC.OS does not cover the 42 glyphs (stated in the README).

## 5. Voice and writing rules

These apply to every piece of text an agent writes for this project: comments, strings,
man pages, dotfiles, docs, commit messages.

- **No em-dashes. At all.** Use a comma, a colon, parentheses or two sentences.
- Write like good documentation: clear, calm, no hype. Explain the why.
- Assume the reader is smart but new.
- Short sentences. No filler. Every sentence earns its place.
- It must not sound machine-written.
- Humour is welcome when it is dry and UNIX-flavoured (the deck: "0 known ways to exit
  vim", "/dev/null: write-only memory, perfect for excuses"). Never at a newcomer's expense.
- Terminal framing is part of the identity: sections as paths (`~/unix_club/06_ricing/`),
  commands as headings (`$ whatis ricing`), a vim-style statusline (`-- NORMAL --`).

## 6. Privacy

- Never write the founders' phone numbers or e-mail addresses anywhere. In the OS's
  club pages they are listed by intra username only.
- The club's source documents stay out of git for this reason. If one is needed in the
  repo, an owner must provide a redacted version.

## 7. Content already in the image

- `~/unix_club/`: the intro deck as text files, mirroring the deck's own tree
  (`01_what_is_unix/` through `07_join_us/`), readable with `cat` at 80 columns.
- `man unix-club`: the club in one manual page.
- `/etc/motd`: the greeting. `~/.shrc`, `/etc/profile`, `~/.config/wm/config`,
  `/usr/share/themes/*.theme`: the dotfiles, commented line by line.
