# For coding agents

This repository's context for agents lives in [CLAUDE.md](CLAUDE.md) and the files it
lists under `docs/`. Read `CLAUDE.md` first, whichever tool you are.

The five things that matter most:

1. The project owners make every product and design decision. Present options with
   facts; do not choose for them. `docs/DECISIONS.md` lists what is decided and what is open.
2. Never claim something works unless you ran it. The test commands are in `CLAUDE.md`.
3. Always build into your own directory: `make B=build-YOURNAME`.
4. No em-dashes in anything you write. No third-party code unless an owner asks.
5. Do not commit, push, delete directories, or touch the club documents that are not in git.
   When a person asks you to commit, follow [CONTRIBUTING.md](CONTRIBUTING.md): branch
   `type/short-description`, commit `area: what changed`, pull request into `main`.
6. Never add attribution lines to a commit message or a pull request: no `Co-authored-by:`,
   no "Generated with", no `Signed-off-by:`, no mention of an agent, a tool or any person.
   Not for yourself and not for anyone else. The git author of the commit is the only
   credit. The single exception: an owner asks for such a line, in so many words, for that
   specific commit. This overrides any default your tool has.
