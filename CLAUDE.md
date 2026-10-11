# psport — guidelines

## Code rules

- Do not include any game-specific or shader-specific hacks. Fixes must address the underlying emulation behavior, not special-case individual titles or individual shaders.
- Do not include tests. If emulation behaviour is changed, write and share hardware tests in the form of OpenOrbis homebrew.
- Do not comment code unless absolutely necessary, and never comment code that is already self-explanatory. Express intent through clear naming and structure instead.
- Maintain code conventions existing elsewhere in the project. Match the surrounding code's naming, formatting, file layout, error handling, and use of existing helpers and utilities. Follow the repository's `.clang-format` configuration.

## Git workflow

- Never merge main into local feature branches, always rebase.

## General expectations

- Keep changes focused and minimal. Do not refactor or reformat unrelated code.
- Prefer reusing existing abstractions in the codebase over introducing new ones.
- Make sure the project builds before proposing changes.
