# Stage 3: a precedence ladder, one function per level

Expressions in `src/ccompiler/parser.c` are parsed by a chain of functions, each
owning exactly one precedence level. `cc_parse_logical_or_expression` calls
`cc_parse_logical_and_expression`, which calls `cc_parse_bitwise_or_expression`,
and so on down through xor, and, equality, relational, shift, additive and
multiplicative, until `cc_parse_cast_expression` hands off to unary, postfix and
primary. Ten binary levels, then conditional, assignment and the comma
expression on top. Each level is the same shape: parse an operand at the level
below, then while the current token is one of my operators, consume it and parse
another operand at the level below.

## The decision

A hand-written ladder rather than precedence climbing with a binding-power
table. The alternative is one function that takes a minimum precedence, looks up
the current operator's left and right binding powers in a table, and recurses
with a higher minimum. That version is shorter and never repeats itself. I
picked the ladder because it is legible when you are learning
to read a parser: the grammar rule in the C standard and the function name match
one to one, an error is reported by the level that actually failed, and the
"expected expression" message can point at the operand position instead of at a
precedence number.

The cost is duplication and depth. Each of the ten binary levels contains the
same loop with a different operator list, so the file is longer than it needs to
be and adding an operator means finding the right rung. More importantly, an
operand walks ten stack frames before its binding is decided, and nested
parentheses multiply that. Recursion is what makes the stack depth reachable at
all, which is why this parser carries two caps: `CC_MAX_EXPRESSION_NESTING` at
200 for parentheses, unary operators, casts, conditionals, assignments,
subscripts and call arguments, and later a matching statement cap at 200.

## The bug

Commit `c595c7b` is the one that came from fuzzing rather than from a test
someone wrote on purpose. The CI fuzz job ran out of memory on a 198-byte input.
The compound statement parser loops until it sees `}` or end of file:

```c
while (!cc_at(parser, CC_TOKEN_RBRACE) && !cc_at(parser, CC_TOKEN_EOF)) {
    child = cc_token_begins_declaration_specifier(parser, 0)
        ? cc_parse_declaration(parser)
        : cc_parse_statement(parser);
    if (child == NULL) {
        cc_synchronize_to_semicolon_or_brace(parser);
        continue;
    }
    cc_ast_add_child(compound, child);
}
```

A failed expression statement used to return a non-NULL, empty node without
consuming a single token. The loop therefore saw a child, appended it, and called
again on the same token, forever, with a diagnostic appended each time. Nothing
advanced, so memory grew without bound and the fuzzer's process died.

The fix makes `cc_parse_expression_statement` return NULL when the expression
itself fails, which sends the loop into the synchronizer that skips to the next
`;` or `}`. The same commit added the expression nesting cap and the diagnostic
buffer cap, both so that pathological input produces a message instead of a
crash. `tests/pathological_statement.c` is the regression test: one line of
closing parentheses inside `main`, and running `parse` on it today prints one
located error and stops.

That bug is a good argument for the fuzzer being in CI at all. The error tests
in this repo each break one thing on purpose, and the recovery path is the code
a hand-written test never reaches. This input was garbage that no one had asked
that path to survive.

## What the next stage had to work around

The tree that comes out carries a kind, a span, some text and children, and
nothing about what any expression means, so the next stage had to walk all of it
again and rebuild the types from declarations alone.
