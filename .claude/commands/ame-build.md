Build Ame with filtered output.

Use:
`scripts/claude/ame-build.sh $ARGUMENTS`

Rules:
- Prefer a target argument if the task is localized, e.g. `anko` or `mochi_initrd`.
- Do not run raw Ninja/CMake unless the wrapper is broken.
- Report only compiler/linker errors and final result.
