#!/bin/sh

# End-to-end assembly back end test: lower a program to x86-64 assembly, hand
# it to the system toolchain through the build driver, run the resulting
# executable and diff its stdout against a committed expectation.

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BINARY="$ROOT_DIR/build/bin/C-Compiler"
EXPECTED_DIR="$ROOT_DIR/tests/expected"
TMP_DIR=$(mktemp -d)

cleanup() {
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM

run_assembly_case() {
    name=$1
    source_file=$2
    expected_exit=$3
    assembly_file="$TMP_DIR/$name.s"
    executable="$TMP_DIR/$name.bin"
    actual_output="$TMP_DIR/$name.out"
    expected_file="$EXPECTED_DIR/$name.out"

    if ! "$BINARY" codegen --target=x86_64 "$ROOT_DIR/$source_file" >"$assembly_file" 2>&1; then
        echo "assembly case '$name': backend failed for $source_file" >&2
        cat "$assembly_file" >&2
        exit 1
    fi

    if ! grep -q "main:" "$assembly_file"; then
        echo "assembly case '$name': no main label in generated assembly" >&2
        exit 1
    fi

    if ! "$BINARY" build "$ROOT_DIR/$source_file" "$executable" >"$TMP_DIR/build.txt" 2>&1; then
        echo "assembly case '$name': assemble/link failed for $source_file" >&2
        cat "$TMP_DIR/build.txt" >&2
        exit 1
    fi

    set +e
    "$executable" >"$actual_output"
    exit_status=$?
    set -e

    if [ "$exit_status" -ne "$expected_exit" ]; then
        echo "assembly case '$name': exit status $exit_status, expected $expected_exit" >&2
        exit 1
    fi

    if ! diff -u "$expected_file" "$actual_output" >"$TMP_DIR/diff.txt" 2>&1; then
        echo "assembly case '$name': wrong program output for $source_file" >&2
        cat "$TMP_DIR/diff.txt" >&2
        exit 1
    fi
}

run_ir_unchanged_case() {
    source_file=$1
    output_file="$TMP_DIR/ir.txt"

    if ! "$BINARY" codegen "$ROOT_DIR/$source_file" >"$output_file" 2>&1; then
        echo "codegen failed for $source_file in textual IR mode" >&2
        cat "$output_file" >&2
        exit 1
    fi

    if ! grep -q "^func main(void) -> int$" "$output_file"; then
        echo "textual IR lost its function header for $source_file" >&2
        exit 1
    fi

    if grep -q "pushq" "$output_file"; then
        echo "textual IR mode emitted assembly for $source_file" >&2
        exit 1
    fi
}

run_diagnostic_case() {
    name=$1
    source_file=$2
    expected_message=$3
    output_file="$TMP_DIR/diagnostic.txt"

    set +e
    "$BINARY" codegen --target=x86_64 "$ROOT_DIR/$source_file" >"$output_file" 2>&1
    status=$?
    set -e

    if [ "$status" -eq 0 ]; then
        echo "assembly case '$name': expected a backend error for $source_file" >&2
        exit 1
    fi

    if ! grep -Fq "$expected_message" "$output_file"; then
        echo "assembly case '$name': missing diagnostic '$expected_message'" >&2
        cat "$output_file" >&2
        exit 1
    fi

    if ! grep -Eq "$source_file:[0-9]+:[0-9]+: error:" "$output_file"; then
        echo "assembly case '$name': diagnostic carries no line and column" >&2
        cat "$output_file" >&2
        exit 1
    fi
}

run_assembly_case asm_hello examples/asm_hello.c 0
run_assembly_case asm_loops examples/asm_loops.c 0
run_assembly_case asm_fib examples/asm_fib.c 0
# feature_showcase prints nothing, so its exit status is the whole result.
run_assembly_case feature_showcase examples/feature_showcase.c 118

run_ir_unchanged_case examples/asm_hello.c
run_ir_unchanged_case examples/feature_showcase.c

run_diagnostic_case local_array tests/asm_local_array.c "does not support local arrays"

echo "assembly end-to-end tests passed"
