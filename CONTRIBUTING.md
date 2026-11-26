# Contributing to UC.OS

Short rules for branches, commits and pull requests. They apply to people and to coding
agents alike. Everything else an agent needs is in [CLAUDE.md](CLAUDE.md) and `docs/`.

## Branches

`main` is protected: nothing is pushed to it directly. Work on a branch and open a pull
request.

Name a branch `type/short-description`, lowercase, words joined by hyphens:

| Type | For | Example |
|---|---|---|
| `feat` | something new | `feat/sound-device` |
| `fix` | a bug | `fix/pipe-wakeup` |
| `docs` | documentation and context files only | `docs/status-update` |
| `test` | tests only | `test/vi-scripts` |
| `chore` | build, tooling, cleanup, no behaviour change | `chore/gitignore-club-docs` |

One topic per branch. Keep branches short-lived.

## Commits

```
area: what changed, in the imperative

Why it changed, and anything a reader needs to know that the diff does not
show. Wrap at 72 columns. The body is optional for small changes.
```

- **area** is the part of the tree you touched, usually a directory or program name:
  `kernel`, `boot`, `libc`, `libgfx`, `libui`, `sh`, `wm`, `term`, `vi`, `htop`, `fetch`,
  `doom`, `tools`, `rootfs`, `build`, `docs`. Use the most specific one that fits. If a
  commit truly spans several, pick the main one or split the commit.
- The first line is at most 72 characters, starts lowercase after the colon, and has no
  full stop. Write "add", "fix", "remove", not "added" or "adds".
- No em-dashes, as everywhere in this project.
- No attribution lines: no `Co-authored-by:`, no "Generated with", no `Signed-off-by:`, no
  mention of a coding agent, a tool or any person. The git author of the commit is the
  only credit. The same goes for pull request titles and descriptions. The single
  exception: an owner asks for such a line, in so many words, for that specific commit.
- A commit should build and keep the tests green on its own.

Examples:

```
kernel: preempt at system call boundaries
sh: stop loops on Ctrl-C
libc: size argv from the real argument count
docs: record the licence decision
```

## Pull requests

- Title: same form as a commit first line.
- Description: what changed and why, how you tested it, and anything not tested.
- Before opening, run `make B=build-NAME test` (kernel, C library, shell and tools) and the
  desktop script listed in [CLAUDE.md](CLAUDE.md),
  and say in the description that they pass.
- If you changed how something works, update the matching file in `docs/` in the same
  pull request. `docs/STATUS.md` almost always needs a line.
- A decision that belongs to the owners (see `docs/DECISIONS.md`) is not made in a pull
  request. Ask first, then record the answer in `docs/DECISIONS.md`.

## Never commit

- The club's source documents (the guidelines, the intro PDF and PowerPoint,
  `unix-club-logo/`): they contain personal contact details. Stage files by path; do not
  use `git add -A` or `git add .` at the repository root.
- Build directories (`build/`, `build-*/`) and Doom game data (`bin/doom/wads/`).

## For coding agents

Follow this file exactly when you are asked to commit, name a branch or open a pull
request. Do not commit, push or open a pull request unless a person asks you to.

Never credit yourself or anyone else in a commit message or a pull request. Many agent
tools add a `Co-authored-by:` or "Generated with" line by default: leave it out, and remove
it if your tool added it. See the rule under Commits above.
