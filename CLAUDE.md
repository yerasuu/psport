# psport — guidelines

## Code rules

- Fixes address the underlying emulation behavior. The core (`src/`, `gpu/shim/`, `gpu/port/`, `gpu/memory/`, `gpu/diag/`, `gpu/shadps4/`, `run.sh`, `build.sh`) stays generic: no special cases for individual titles or individual shaders. Optimisations that only help one game go in that game's profile (`gpu/games/<game>/`). A correction one title needs before the general fix exists is allowed only as a temporary `WORKAROUND` quirk (decision #331, `docs/en/QUIRKS.md`): declared in the registry with its reason and a reference to the kb task seeking the general fix, activated only by that title's `Game::Title`, its code in `gpu/games/<game>/`, logged and disableable with `PS_QUIRKS`, and removed with its code when the general fix lands.
- Do not include tests. If emulation behaviour is changed, write and share hardware tests in the form of OpenOrbis homebrew.
- Do not comment code unless absolutely necessary, and never comment code that is already self-explanatory. Express intent through clear naming and structure instead.
- Maintain code conventions existing elsewhere in the project. Match the surrounding code's naming, formatting, file layout, error handling, and use of existing helpers and utilities. Follow the repository's `.clang-format` configuration.

## Native implementation

- The goal is every system library implemented natively in the port (HLE). The user's dumped
  system modules may be used to test, to detect how a library behaves, or as a temporary patch
  while the native version is missing (`PS_LLE`), but never as the solution.
- To learn how the firmware or a game really does something (parameter layouts, defaults, error
  codes), read the original code with the `rea` subagent (REA over Ghidra, user-level agent that
  starts its MCP server only while it runs; needs `ghidra` and `jdk21` in the nix profile) instead
  of guessing from shadPS4 or raw `objdump`. Its findings become kb notes or tasks.
- Optimisations that only help one game live in that game's profile (`gpu/games/<game>/`,
  `Game::Profile`); the core keeps the generic mechanism and runs every game correctly without a
  profile (`PS_GAME_PROFILE=none`). A profile may speed a game up, never make it work; what makes
  one game work before the general fix is a `WORKAROUND` quirk in its folder (see Code rules).
- Nothing silent: an unimplemented, stubbed or failing call stays visible in the log and in the
  call report with its count. Logs are technical English.

## Naming (C and C++)

Role prefixes (the original "Apps Hungarian": what a value means, not its type) and scope
prefixes, in all of psport's own code (`src/`, `gpu/shim/`, `gpu/port/`, `gpu/memory/`,
`gpu/diag/`, `gpu/layer/`, `gpu/games/`, `gpu/pngenc/` and the other `gpu/` libraries psport
adds). No type-width prefixes (`u32`, `i64`, `f32`, `n`, `st`...): the compiler already knows the
type, and the name would lie once it changes (C++ Core Guidelines NL.5). The inherited shadPS4
tree (`gpu/shadps4/`) keeps its names, except the lines psport adds. SDK/ABI names (exports,
structures the game reads, C++ classes it links against) stay as defined. Existing code is
converted area by area in its own commits; new and touched code follows this now.

- Case as the surrounding code: `snake_case` for variables, parameters and members in C and in
  psport's C++ (as shadPS4 does); functions start with a verb (predicates read as questions),
  types keep the file's case, macros and constants are `UPPER_SNAKE`. The prefix is the first word
  of the snake_case name.

| Prefix | Role | Example |
|---|---|---|
| `cb_` | size in bytes | `cb_image` |
| `c_` | count of elements | `c_modules` |
| `i_` | index (bare `i`, `j` for short loops) | `i_segment` |
| `off_` | offset (with its unit when not bytes: `off_dw_`) | `off_data` |
| `va_` | address held as an integer (`uintptr_t`, `u64`; guest or host) | `va_guest` |
| `h_` | handle or descriptor (fd, id an API hands out) | `h_file` |
| `sz_` | NUL-terminated string | `sz_path` |
| `p_` / `pp_` | raw pointer / pointer to pointer | `p_vma` |
| `pfn_` | function pointer | `pfn_handler` |

- Booleans read as predicates instead of a prefix: `is_`, `has_`, `can_`, `should_`
  (`is_mapped`).
- Scope before the role: `g_` globals, `s_` file-scope statics, `m_` C++ class members
  (`g_cb_arena`, `s_p_trap_table`, `m_is_running`). C struct fields take only the role prefix.
- Output parameters add `out_` after the role: `p_out_count`.
- Units in the name when a number has one and no prefix gives it: `timeout_us`, `rate_hz`.
- Whole words after the prefix, no invented abbreviations; conventional ones (`len`, `ctx`, `fd`,
  `tid`) are fine. Prefer `std::unique_ptr` for owning pointers in C++; a raw owning pointer says
  so in its contract.

## Git workflow

- Never merge main into local feature branches, always rebase.
- Branches: `main` (what users get, a tag per version), `develop` (integration),
  `<type>/<kb id>-<topic>` per task; into `develop` by rebase, squash and fast-forward; into
  `main` only after a clean night run. The user pushes.
- One useful commit per task, carrying its docs, FORK.md rows and comments; always Conventional
  Commits `type(area): subject` (imperative English, ~50 characters, 72 at most; type and area from
  the board tags: psport, loader, kernel, system, gpu, audio, video, input, net, regress,
  toolchain) with an optional one-to-four-line body, after a blank line, giving the why;
  `BREAKING CHANGE:` footer when it breaks launchers or settings; no AI attribution; signed.
- psport is an independent project, not a fork: never merge bbport or shadPS4 wholesale. Review
  them from time to time and port selected changes by hand, each recorded in `FORK.md` with its
  source commit; keep `FORK.md`'s last reviewed points and `gpu/VENDOR.txt` current. No upstream
  PRs.

## General expectations

- Keep changes focused and minimal. Do not refactor or reformat unrelated code.
- Prefer reusing existing abstractions in the codebase over introducing new ones.
- Make sure the project builds before proposing changes.
