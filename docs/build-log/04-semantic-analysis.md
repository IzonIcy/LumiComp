# Stage 4: types live in the checker, not in the tree

Semantic analysis is a second full walk over the parser's tree. It opens a
scope, walks declarations and statements, and for every expression it computes
what that expression means into a local value:

```c
typedef struct {
    const CCSemaType *type;
    bool is_lvalue;
    bool is_function_designator;
    bool ok;
} CCSemaExpr;
```

That struct is passed up the recursion and then discarded. The tree itself never
learns anything.

## The decision

The AST stays untyped. `CCAstNode` is a kind, a span, a `char *text`, and a list
of children. Types, scopes, labels, goto targets and switch case values are all
owned by `CCSemaContext` while the pass runs and freed when it ends. The
alternative was to hang a `const CCSemaType *` on every expression node, so that
code generation and the assembly backend could read back exactly what the
checker had already worked out.

I chose the untyped tree because it keeps one job per file. The parser cannot
report a type error, so it never has to build a type table while it is still
guessing at declarator syntax. Sema can be skipped entirely, and
`--dump-ast-json` prints syntax rather than a mix of syntax and resolved types.
Freshly allocated nodes have one shape, and printing the tree stays one short
function in `parser_ast.c`.

The cost lands on everyone downstream, and it is the sort of cost that turns
into bugs. Because the check result evaporates, code generation has to render a
declaration back into text when it writes the IR header. That is why the IR for
a function typedef'd as `u64` comes out spelled the way it was written:

```text
func half(v: u64) -> u64
```

The IR is not wrong, but it is unresolved. So the assembly backend re-collects
typedef spellings from the parse result (`cc_asm_collect_typedefs`) and rebuilds
types by re-parsing those strings (`cc_asm_type_from_text`). Sema, codegen and
the assembly backend each have an opinion about what `unsigned long` means, and
nothing forces the three to agree. Sema's own description of a type is lossy in
the same way: `cc_type_name` answers `pointer` for any pointer and `struct` for
any record, which is how a diagnostic ends up reading
`invalid cast from pointer to pointer` without naming either type.

For the record, the machinery is ordinary: a stack of scopes, each a flat array
searched linearly by name, with record tags kept in a separate table from
ordinary identifiers. Diagnostics accumulate into a buffer with a severity field
rather than aborting at the first error, and each stage caps its buffer at
`CC_MAX_DIAGNOSTICS` (200) so an error cascade cannot eat memory.

## The bug

Case labels have to be integer constant expressions, so the checker evaluates
them, and that evaluation sits in `sema_const.c` behind
`cc_eval_const_integer_expr`. Codegen borrows the same function when it lowers a
`switch`, so one file decides what every constant in the program is.

Commit `61fb763` fixed the fold. `cc_parse_integer_literal` turned binary digits
into a value like this:

```c
parsed = (parsed << 1) | (copy[index] - '0');
```

with `parsed` declared as a signed `long long`. For a literal of 64 digits the
last shift moves a bit into the sign position, and shifting a signed value into
its own sign bit is undefined behavior in C, not a wraparound with a defined
result. UBSan flagged it at the shift on the all-ones case. The fix accumulates
into an `unsigned long long` and converts once on the way out, which leaves the
value identical: 64 binary ones still fold to -1, and `codegen` on
`tests/binary_literal_overflow.c` lowers its case to `t0 = eq 0, -1`.

It is a checker bug, and worth naming as one. The scanner had already accepted
the token correctly. What broke was the evaluation of a constant, in the stage
whose job is to say what a construct means. A sanitizer found it rather than a
test, because the folded value came out right in builds where the optimizer had
not yet acted on the undefined behavior.

## What the next stage had to work around

Code generation inherited the gap above: with no types on the tree it could only
print the type names it could render from declaration text, and trust the
assembly backend to work out the rest.
