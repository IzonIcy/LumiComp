#!/bin/sh

# Walk examples/feature_showcase.c through the front end, the textual IR and
# the x86-64 back end. Run `make` first; this script assumes a built compiler.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BINARY="$ROOT_DIR/build/bin/C-Compiler"
SOURCE="$ROOT_DIR/examples/feature_showcase.c"

if [ ! -x "$BINARY" ]; then
    echo "error: build/bin/C-Compiler is missing, run 'make' first" >&2
    exit 1
fi

echo "== check"
"$BINARY" check "$SOURCE"

echo
echo "== codegen (textual IR)"
"$BINARY" codegen "$SOURCE"

echo
echo "== codegen --target=x86_64 (assembly)"
"$BINARY" codegen --target=x86_64 "$SOURCE"
