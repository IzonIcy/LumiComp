# Stage 5: the IR is text, and the backend reads it back

There are two backends now. `codegen` lowers the checked tree to a line-oriented
text IR. `x86.c` takes that text and produces assembly:

```c
cc_x86_translation_unit(&parse_result, codegen_result.text, &x86_result);
```

Note the second argument. The assembly backend does not receive an instruction
list. It receives a string, splits it into lines, and dispatches on the first
word of each one.

## The decision

The IR is a printable program and the backend is a reader of it. Codegen emits
things like `t3 = shl flags, 1` and `cjump t2, for.body.1, for.end.3`, and
`x86.c` re-parses those lines into slices, literals and symbols. The alternative
was a shared in-memory instruction struct: codegen fills an array, both backends
walk the array, and the text becomes one optional printer among several
consumers.

I kept the text because it makes the pipeline inspectable at every point. The
IR is the artifact the golden tests diff against (`tests/golden/*.ir`), it is
what the README demo shows, and writing the assembly backend later needed no change
to `codegen.c` at all. It also keeps the two halves honest about their
contract: if a fact is not printed, the backend cannot see it.

That last part is the cost, and it is steep. The IR carries no type objects,
only the type names codegen could render, so `x86.c` reconstructs types from the
declarations in the parse tree and from strings like `u64` that it looks up in a
typedef table it rebuilds for itself. Every operand is a `CCAsmSlice` of
characters, and every literal is parsed out of that text when the backend
reaches it. A type sema knows as `unsigned long` arrives at the backend as the
six characters `u64` if the source used a typedef, and `pointer` if it was a
pointer, and has to be decoded again.

## The bug

Commit `9a01789` is titled "stop dropping a value's declared type in the
assembly back end". It is one bug in three places, all of them in `x86.c`.

A function definition never published its own signature. Declarations registered
their return types, but a definition did not, so a call to a function defined
earlier in the file was assumed to return a 32-bit `int`, and a 64-bit return
value came back truncated. The fix is one call at the top of
`cc_asm_emit_function`:

```c
cc_asm_register_function(context, name, return_type, false);
```

A literal store wrote the destination's type directly:

```c
destination->type = cc_asm_type_make(CC_ASM_SCALAR_I64, 0);
```

which overwrote the type the destination was declared with. Storing an integer
literal into an `unsigned` variable therefore turned the rest of that variable's
life signed, and later arithmetic and comparisons widened it the wrong way. The
fix routes it through `cc_asm_set_destination_type`, which leaves a declared
type alone.

The third one is the ugliest. `cc_asm_operand_type` freed a parsed literal and
then read `literal.integer_unsigned` out of it to decide between `U64` and `I64`.
The fix copies that flag into a local before the free.

All three are the same shape: the backend did not know the type, so it guessed,
and the guess was recorded in a place that other code trusted. When the only
type information you have is text plus a declaration table you assembled
yourself, "the type is whatever I inferred here" has to be decided in many
functions, and three of them got it wrong.

## What is still missing

Structs by value, local arrays, single-precision floats, `sizeof`, and more than
six integer call arguments are all rejected with a located diagnostic rather
than silently miscompiled. A local array gives
`x86-64 backend does not support local arrays yet ('a')`, and `sizeof(int)`
stops in codegen with `unsupported expression in code generation` before the
backend ever sees it. There is no include search path, so `#include <stdio.h>`
fails to open. Bit-fields, flexible array members and function pointers are
incomplete. There are no optimization passes: folding constant integer
expressions during lowering is the only work above the baseline. Every value
also lives in a stack slot, with no register allocation, so a one-statement
function still spills its argument to `-8(%rbp)` and reloads its result before
returning. The output is correct before it is fast, which is the trade the
README states plainly.

The first thing the next stage would have to work around is the one this post
started with: a register allocator, a typed IR, or a second target all need
facts the current IR does not print, and anything left unprinted will be
recovered from the parse tree by guesswork exactly like the three bugs above.
