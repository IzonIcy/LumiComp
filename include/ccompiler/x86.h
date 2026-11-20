#ifndef CCOMPILER_X86_H
#define CCOMPILER_X86_H

#include "ccompiler/lexer.h"
#include "ccompiler/parser.h"

typedef struct {
    char *text;
    CCDiagnosticBuffer diagnostics;
} CCX86Result;

/* Lowers the textual IR emitted by cc_codegen_translation_unit to x86-64
 * AT&T assembly. The parse result supplies typedef spellings that the IR
 * leaves unresolved. */
void cc_x86_translation_unit(const CCParseResult *parse_result, const char *ir_text, CCX86Result *result);
void cc_x86_result_free(CCX86Result *result);

#endif
