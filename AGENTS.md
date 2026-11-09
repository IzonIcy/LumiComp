# LumiComp

A C compiler written in C. Pipeline: lex, preprocess, parse, semantic analysis, lower to textual IR. It is a subset compiler, not an ISO C implementation.

## Commands

```sh
make          # build -> build/bin/C-Compiler
make test     # smoke suites + golden IR regression
make analyze  # clang static analyzer
make verify   # both
make fuzz     # ASan/UBSan harness, then: ./build/bin/fuzz-standalone examples/*.c
make sanitizers
```

Regenerate golden files only after an intentional codegen change:

```sh
UPDATE_GOLDEN=1 sh tests/golden_smoke.sh
```

## Layout

```
src/ccompiler/   compiler sources, one concern per file
include/         shared headers
tests/           shell-driven smoke suites, golden files, data-driven cases
fuzz/            libFuzzer and standalone harnesses
examples/        sample C programs used by tests and demos
```

## Conventions

- C11, compiled with `-Wall -Wextra -Wpedantic`. A new warning is a bug.
- Diagnostics carry source locations. Do not add a diagnostic without a line and column.
- Memory ownership is explicit. If a function returns a pointer, its owner is documented at the declaration.
- New syntax support needs a test in `tests/` covering both success and the diagnostic path.

## Verification

`make verify` before reporting done. For a parser or semantic change, also run the fuzzer for a short window and confirm no crash. Golden diffs are expected only when you changed codegen on purpose; read the diff before accepting it.
