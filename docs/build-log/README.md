# LumiComp build log

Five posts, one per stage of the pipeline, each built around a single design
choice and a single bug that the code still carries the marks of. They are
written for someone who wants to read a compiler in C, has opinions about how
one should be built, and would rather see a real trade and a real failure than a
tour of the feature list.

1. [The lexer](01-lexer.md): tokens are offsets into the source rather than
   owned text, which pushes spellings and literal values onto every later stage,
   and the only defect that ever stopped this stage from running was a rename
   that broke the build for one commit.
2. [The preprocessor](02-preprocessor.md): it rewrites the program as text
   instead of as tokens, which costs honest source locations, and includes had
   no depth limit until a pair of mutually including headers recursed until the
   stack died.
3. [The parser](03-parser.md): one function per precedence level keeps the
   grammar readable and the stack deep, and a failed expression statement that
   returned a node without consuming a token spun forever on a 198-byte fuzzer
   input.
4. [Semantic analysis](04-semantic-analysis.md): types are computed in the
   checker and dropped instead of being attached to the tree, so later stages
   re-derive them, and the constant folder in the same stage shifted a 64-digit
   binary literal into its own sign bit.
5. [The IR and the backend](05-ir-and-what-comes-next.md): the IR is printed
   text that the assembly backend parses back, which keeps it diffable and makes
   the backend guess at types, three guesses of which were wrong until they were
   fixed.

Every bug named here cites the commit that fixed it. Read the posts in order if
you want the pipeline, or jump to the stage you are writing yourself.
