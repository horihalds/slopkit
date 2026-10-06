# AGENTS.md

This file is the AI agent's operating guide for this repository: the rules and
conventions the project owner has mandated, plus a map of which document answers
which question. Follow it for every task.

## Start here: which file answers what

Read the routing table first, then only the file it points at.

| Your task | Read | That file owns |
| --- | --- | --- |
| Find where something lives, or the project conventions and gotchas | [`docs/ARCHITECTURE_MAP.md`](docs/ARCHITECTURE_MAP.md) | Locations, conventions, gotchas |
| Build, install, run or diagnose tests | [`docs/TESTING.md`](docs/TESTING.md) | Every command, recipe and the flake procedure |
| Understand the plugin model, the ABI or the debugger mechanism | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Mechanism prose |
| Change the UI | [`docs/UI_DESIGN.md`](docs/UI_DESIGN.md) | UI rules, including the target-access rule |
| Add or change a log record | [`docs/LOGGING.md`](docs/LOGGING.md) | Log levels, categories, message style |
| A test failed in a full run | [`docs/KNOWN_ISSUES.md`](docs/KNOWN_ISSUES.md) | Accepted flakes and their lone-run commands |
| The product, install or headless CLI | [`README.md`](README.md) | User overview, dependency list, flags |

## Working rules

- Rely on `docs/ARCHITECTURE_MAP.md`; do not rediscover files unnecessarily, and
  search widely only when the map does not answer where something is.
- Limit exploration to ~25 tool calls, then ask at most 3 questions or propose a plan.
- Do not re-read a file or re-run `git log`/`git status` you already have in context.
- Pipe wide searches and scripts through `| head -40`.
- Never perform target access on the UI thread; the authoritative wording of the
  rule is in `docs/UI_DESIGN.md`.
- A fact that has an owning file appears only there. When it changes, update the
  owner in the same task; do not restate it somewhere else.

## Change scope and install rule

- When a completed plan changes anything that ships in the installed build —
  `src/**`, `CMakeLists.txt`, `cmake/**`, `assets/**` or `data/**` — run
  `./tools/install.sh` so the owner can try the installed build. Plans that only
  touch `tests/**`, `README.md`, `AGENTS.md`, `docs/**`, formatting or comments
  skip it. If `./tools/install.sh` fails, report the failing command and its
  output and still finish the plan.
- Never add a dependency without consulting the owner first; the current
  dependency list and packaging notes live in `README.md`.

## Temporary files

- Create temporary files under the project's `tmp/` folder, not the system temp
  folder; `tmp/` is gitignored.
- Empty `tmp/` of any remaining scratch files when a plan completes.

## Reference tree

- Everything under `reference/` is read-only; never modify anything under it.

## Version control

- When a plan is completed, automatically create a git commit for the changes.
- If the current branch has an upstream, also push the commit.

## Definition of done

1. `tools/verify.sh` all green — format, warning-free build and tests (see
   `docs/TESTING.md`).
2. A flaky failure is acceptable only if it is listed in `docs/KNOWN_ISSUES.md`
   AND the test passes alone under `ctest -R`. Add new flakes to that file in the
   same commit.
3. Docs/map updated; `tmp/` emptied; then commit (and push if upstream exists).

## Keeping the docs true

- Treat the documentation as living: when future work changes the project's
  rules, conventions or toolchain, update the owning file in the same task.
- Relative doc links and `docs/<file>.md#<anchor>` citations are guarded by the
  `docs-links-check` CTest test (`tools/docs-check.sh`, see `docs/TESTING.md`);
  keep them resolving.
