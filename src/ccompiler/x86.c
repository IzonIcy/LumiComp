#include "ccompiler/x86.h"
#include "ccompiler/util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The backend emits Mach-O flavoured AT&T assembly when it is built on Darwin
 * and ELF flavoured assembly elsewhere, so the driver can hand the output
 * straight to the host toolchain. */
#ifdef __APPLE__
#define CC_ASM_GLOBAL_PREFIX "_"
#define CC_ASM_RODATA_HEADER ".section __TEXT,__cstring,cstring_literals"
#else
#define CC_ASM_GLOBAL_PREFIX ""
#define CC_ASM_RODATA_HEADER ".section .rodata"
#endif

#define CC_ASM_MAX_CALL_REGISTERS 8

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} CCAsmBuilder;

typedef struct {
    const char *start;
    const char *end;
} CCAsmSlice;

typedef enum {
    CC_ASM_SCALAR_VOID = 0,
    CC_ASM_SCALAR_BOOL,
    CC_ASM_SCALAR_I8,
    CC_ASM_SCALAR_U8,
    CC_ASM_SCALAR_I16,
    CC_ASM_SCALAR_U16,
    CC_ASM_SCALAR_I32,
    CC_ASM_SCALAR_U32,
    CC_ASM_SCALAR_I64,
    CC_ASM_SCALAR_U64,
    CC_ASM_SCALAR_F32,
    CC_ASM_SCALAR_F64,
    CC_ASM_SCALAR_AGGREGATE
} CCAsmScalar;

typedef struct {
    CCAsmScalar scalar;
    size_t pointer_depth;
} CCAsmType;

typedef struct {
    char *name;
    CCAsmType type;
    int slot;
    char *global_label;
    bool declared;
    bool has_initializer;
    long long initializer;
} CCAsmSymbol;

typedef struct {
    char *name;
    char *spelled;
    size_t pointer_depth;
} CCAsmTypedef;

typedef struct {
    char *name;
    CCAsmType return_type;
    bool variadic;
} CCAsmFunction;

typedef struct {
    char *label;
    char *bytes;
    size_t length;
} CCAsmString;

typedef struct {
    char *label;
    uint64_t bits;
} CCAsmDouble;

typedef enum {
    CC_ASM_LITERAL_NONE = 0,
    CC_ASM_LITERAL_INTEGER,
    CC_ASM_LITERAL_FLOAT,
    CC_ASM_LITERAL_STRING
} CCAsmLiteralKind;

typedef struct {
    CCAsmLiteralKind kind;
    long long integer;
    bool integer_unsigned;
    double floating;
    char *bytes;
    size_t length;
} CCAsmLiteral;

typedef struct {
    const CCParseResult *parse_result;
    CCDiagnosticBuffer diagnostics;
    CCAsmBuilder output;
    CCAsmBuilder rodata;
    CCAsmSymbol *locals;
    size_t local_count;
    size_t local_capacity;
    CCAsmSymbol *globals;
    size_t global_count;
    size_t global_capacity;
    CCAsmTypedef *typedefs;
    size_t typedef_count;
    size_t typedef_capacity;
    CCAsmFunction *functions;
    size_t function_count;
    size_t function_capacity;
    CCAsmString *strings;
    size_t string_count;
    size_t string_capacity;
    CCAsmDouble *doubles;
    size_t double_count;
    size_t double_capacity;
    int slot_count;
    size_t scratch_count;
    size_t scratch_used;
    char *function_name;
    const CCAstNode *function_node;
    CCAsmType function_return_type;
    bool failed;
} CCAsmContext;

static const char *cc_asm_int_argument_32(int index);
static const CCAstNode *cc_asm_find_function_node(const CCParseResult *parse_result, const char *name);
static bool cc_asm_is_place_text(const char *text);

static const char *const CC_ASM_INT_REGISTERS[7] = {"%rax", "%rcx", "%rdx", "%r8", "%r9", "%r10", "%r11"};
static const char *const CC_ASM_INT_ARGUMENTS[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
static const char *const CC_ASM_INT_32[7] = {"%eax", "%ecx", "%edx", "%r8d", "%r9d", "%r10d", "%r11d"};

static void cc_asm_builder_append_range(CCAsmBuilder *builder, const char *text, size_t length) {
    size_t required;

    required = builder->length + length + 1;
    if (required > builder->capacity) {
        size_t new_capacity;

        new_capacity = builder->capacity == 0 ? 256 : builder->capacity;
        while (new_capacity < required) {
            new_capacity *= 2;
        }

        builder->data = cc_reallocate_or_die(builder->data, new_capacity);
        builder->capacity = new_capacity;
    }

    if (length > 0) {
        memcpy(builder->data + builder->length, text, length);
    }
    builder->length += length;
    builder->data[builder->length] = '\0';
}

static void cc_asm_builder_append_cstring(CCAsmBuilder *builder, const char *text) {
    cc_asm_builder_append_range(builder, text, strlen(text));
}

static char *cc_asm_builder_take(CCAsmBuilder *builder) {
    char *data;

    if (builder->data == NULL) {
        data = cc_duplicate_string("");
    } else {
        data = builder->data;
    }

    builder->data = NULL;
    builder->length = 0;
    builder->capacity = 0;
    return data;
}

static void cc_asm_emit_line(CCAsmBuilder *builder, const char *format, ...) {
    char stack_buffer[512];
    char *heap_buffer;
    int needed;
    va_list args;
    va_list copy;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);

    if (needed < 0) {
        va_end(copy);
        return;
    }

    if ((size_t)needed < sizeof(stack_buffer)) {
        cc_asm_builder_append_cstring(builder, stack_buffer);
    } else {
        heap_buffer = cc_reallocate_or_die(NULL, (size_t)needed + 1);
        vsnprintf(heap_buffer, (size_t)needed + 1, format, copy);
        cc_asm_builder_append_cstring(builder, heap_buffer);
        free(heap_buffer);
    }
    va_end(copy);

    cc_asm_builder_append_range(builder, "\n", 1);
}

static void cc_asm_configure_diagnostic(CCAsmContext *context, CCSpan span, const char *message) {
    size_t index;

    if (!cc_diagnostic_buffer_can_add(&context->diagnostics, CC_DIAGNOSTIC_ERROR)) {
        free((char *)message);
        return;
    }

    if (context->diagnostics.count == context->diagnostics.capacity) {
        size_t new_capacity;

        new_capacity = context->diagnostics.capacity == 0 ? 8 : context->diagnostics.capacity * 2;
        context->diagnostics.items = cc_reallocate_or_die(context->diagnostics.items, new_capacity * sizeof(*context->diagnostics.items));
        context->diagnostics.capacity = new_capacity;
    }

    if (span.length == 0 && span.offset < context->parse_result->source.length) {
        span.length = 1;
    }

    index = context->diagnostics.count;
    context->diagnostics.items[index].span = span;
    context->diagnostics.items[index].path = NULL;
    context->diagnostics.items[index].message = (char *)message;
    context->diagnostics.items[index].severity = CC_DIAGNOSTIC_ERROR;
    context->diagnostics.count++;
}

static void cc_asm_error(CCAsmContext *context, const char *format, ...) {
    char stack_buffer[256];
    char *message;
    int needed;
    CCSpan span;
    va_list args;
    va_list copy;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(stack_buffer, sizeof(stack_buffer), format, args);
    va_end(args);

    if (needed < 0) {
        va_end(copy);
        message = cc_duplicate_string("diagnostic formatting failure");
    } else if ((size_t)needed < sizeof(stack_buffer)) {
        va_end(copy);
        message = cc_duplicate_string(stack_buffer);
    } else {
        message = cc_reallocate_or_die(NULL, (size_t)needed + 1);
        vsnprintf(message, (size_t)needed + 1, format, copy);
        va_end(copy);
    }

    span.offset = 0;
    span.length = 0;
    if (context->function_node != NULL) {
        span = context->function_node->span;
    } else if (context->parse_result->translation_unit != NULL) {
        span = context->parse_result->translation_unit->span;
    }

    cc_asm_configure_diagnostic(context, span, message);
    context->failed = true;
}

static void cc_asm_trim(CCAsmSlice *slice) {
    while (slice->start < slice->end && (*slice->start == ' ' || *slice->start == '\t')) {
        slice->start++;
    }
    while (slice->end > slice->start && (slice->end[-1] == ' ' || slice->end[-1] == '\t' || slice->end[-1] == '\r')) {
        slice->end--;
    }
}

static bool cc_asm_slice_equals(CCAsmSlice slice, const char *text) {
    size_t length;

    length = strlen(text);
    if ((size_t)(slice.end - slice.start) != length) {
        return false;
    }

    return memcmp(slice.start, text, length) == 0;
}

static char *cc_asm_slice_dup(CCAsmSlice slice) {
    return cc_duplicate_range(slice.start, (size_t)(slice.end - slice.start));
}

static size_t cc_asm_split_list(CCAsmSlice slice, CCAsmSlice *items, size_t maximum) {
    size_t count;
    const char *start;
    const char *cursor;
    int depth;
    char quote;

    count = 0;
    start = slice.start;
    depth = 0;
    quote = '\0';

    for (cursor = slice.start; cursor < slice.end; cursor++) {
        char ch;

        ch = *cursor;
        if (quote != '\0') {
            if (ch == '\\' && cursor + 1 < slice.end) {
                cursor++;
                continue;
            }
            if (ch == quote) {
                quote = '\0';
            }
            continue;
        }

        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '(' || ch == '[' || ch == '{') {
            depth++;
            continue;
        }
        if (ch == ')' || ch == ']' || ch == '}') {
            depth--;
            continue;
        }
        if (ch == ',' && depth == 0) {
            if (count >= maximum) {
                return count;
            }
            items[count].start = start;
            items[count].end = cursor;
            cc_asm_trim(&items[count]);
            count++;
            start = cursor + 1;
        }
    }

    if (count < maximum) {
        items[count].start = start;
        items[count].end = slice.end;
        cc_asm_trim(&items[count]);
        count++;
    }

    return count;
}

static bool cc_asm_parse_integer_text(const char *text, long long *value_out, bool *unsigned_out) {
    char buffer[80];
    size_t length;
    size_t digits_end;
    bool is_unsigned;
    bool negative;
    const char *digits;
    unsigned long long parsed;
    char *end_pointer;
    int base;

    length = strlen(text);
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length + 1);

    is_unsigned = false;
    digits_end = length;
    while (digits_end > 0) {
        char ch;

        ch = buffer[digits_end - 1];
        if (ch == 'u' || ch == 'U') {
            is_unsigned = true;
            digits_end--;
            continue;
        }
        if (ch == 'l' || ch == 'L') {
            digits_end--;
            continue;
        }
        break;
    }
    buffer[digits_end] = '\0';

    digits = buffer;
    negative = false;
    if (*digits == '+' || *digits == '-') {
        negative = *digits == '-';
        digits++;
    }
    if (*digits == '\0') {
        return false;
    }

    base = 10;
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        base = 16;
        digits += 2;
    } else if (digits[0] == '0' && (digits[1] == 'b' || digits[1] == 'B')) {
        size_t index;
        unsigned long long accumulator;

        digits += 2;
        if (*digits == '\0') {
            return false;
        }
        accumulator = 0;
        for (index = 0; digits[index] != '\0'; index++) {
            if (digits[index] != '0' && digits[index] != '1') {
                return false;
            }
            accumulator = (accumulator << 1) | (unsigned long long)(digits[index] - '0');
        }
        *value_out = negative ? -(long long)accumulator : (long long)accumulator;
        *unsigned_out = is_unsigned;
        return true;
    } else if (digits[0] == '0') {
        base = 8;
    }

    parsed = strtoull(digits, &end_pointer, base);
    if (end_pointer == digits || *end_pointer != '\0') {
        return false;
    }

    *value_out = negative ? -(long long)parsed : (long long)parsed;
    *unsigned_out = is_unsigned || (parsed > 0x7fffffffffffffffULL);
    return true;
}

static bool cc_asm_looks_like_float(const char *text) {
    const char *cursor;
    bool hexadecimal;

    cursor = text;
    if (*cursor == '+' || *cursor == '-') {
        cursor++;
    }

    hexadecimal = cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X');
    for (; *cursor != '\0'; cursor++) {
        if (*cursor == '.') {
            return true;
        }
        if (hexadecimal) {
            if (*cursor == 'p' || *cursor == 'P') {
                return true;
            }
        } else if (*cursor == 'e' || *cursor == 'E') {
            return true;
        }
    }

    return false;
}

static bool cc_asm_parse_float_text(const char *text, double *value_out) {
    char buffer[128];
    size_t length;
    char *end_pointer;

    length = strlen(text);
    if (length == 0 || length >= sizeof(buffer)) {
        return false;
    }
    memcpy(buffer, text, length + 1);

    if (buffer[length - 1] == 'f' || buffer[length - 1] == 'F') {
        if (!cc_asm_looks_like_float(buffer)) {
            return false;
        }
        buffer[length - 1] = '\0';
    }

    *value_out = strtod(buffer, &end_pointer);
    return end_pointer != buffer && *end_pointer == '\0';
}

static void cc_asm_literal_free(CCAsmLiteral *literal) {
    free(literal->bytes);
    memset(literal, 0, sizeof(*literal));
}

static char *cc_asm_decode_string(CCAsmSlice text, size_t *length_out) {
    const char *cursor;
    CCAsmBuilder builder;

    memset(&builder, 0, sizeof(builder));

    cursor = text.start;
    while (cursor < text.end && *cursor != '"') {
        cursor++;
    }
    if (cursor >= text.end) {
        return NULL;
    }
    cursor++;

    while (cursor < text.end) {
        char ch;

        ch = *cursor;
        if (ch == '"') {
            break;
        }
        if (ch != '\\') {
            cc_asm_builder_append_range(&builder, cursor, 1);
            cursor++;
            continue;
        }

        cursor++;
        if (cursor >= text.end) {
            break;
        }
        ch = *cursor;
        cursor++;
        switch (ch) {
            case 'n': cc_asm_builder_append_range(&builder, "\n", 1); break;
            case 't': cc_asm_builder_append_range(&builder, "\t", 1); break;
            case 'r': cc_asm_builder_append_range(&builder, "\r", 1); break;
            case 'a': cc_asm_builder_append_range(&builder, "\a", 1); break;
            case 'b': cc_asm_builder_append_range(&builder, "\b", 1); break;
            case 'f': cc_asm_builder_append_range(&builder, "\f", 1); break;
            case 'v': cc_asm_builder_append_range(&builder, "\v", 1); break;
            case 'x': {
                unsigned int value;
                int digits;

                value = 0;
                digits = 0;
                while (cursor < text.end && digits < 2) {
                    char hex;

                    hex = *cursor;
                    if (hex >= '0' && hex <= '9') {
                        value = value * 16 + (unsigned int)(hex - '0');
                    } else if (hex >= 'a' && hex <= 'f') {
                        value = value * 16 + (unsigned int)(hex - 'a' + 10);
                    } else if (hex >= 'A' && hex <= 'F') {
                        value = value * 16 + (unsigned int)(hex - 'A' + 10);
                    } else {
                        break;
                    }
                    cursor++;
                    digits++;
                }
                if (digits > 0) {
                    char byte;

                    byte = (char)(value & 0xff);
                    cc_asm_builder_append_range(&builder, &byte, 1);
                }
                break;
            }
            default:
                if (ch >= '0' && ch <= '7') {
                    unsigned int value;
                    int digits;

                    value = (unsigned int)(ch - '0');
                    digits = 1;
                    while (cursor < text.end && digits < 3 && *cursor >= '0' && *cursor <= '7') {
                        value = value * 8 + (unsigned int)(*cursor - '0');
                        cursor++;
                        digits++;
                    }
                    {
                        char byte;

                        byte = (char)(value & 0xff);
                        cc_asm_builder_append_range(&builder, &byte, 1);
                    }
                } else {
                    cc_asm_builder_append_range(&builder, &ch, 1);
                }
                break;
        }
    }

    if (builder.data == NULL) {
        builder.data = cc_duplicate_string("");
        builder.length = 0;
    }
    *length_out = builder.length;
    return builder.data;
}

static bool cc_asm_is_string_literal(CCAsmSlice text) {
    const char *cursor;

    cursor = text.start;
    if (cursor < text.end && (*cursor == 'u' || *cursor == 'U' || *cursor == 'L')) {
        cursor++;
        if (cursor < text.end && *cursor == '8') {
            cursor++;
        }
    }

    return cursor < text.end && *cursor == '"';
}

static void cc_asm_parse_literal(CCAsmSlice text, CCAsmLiteral *literal) {
    char *buffer;

    memset(literal, 0, sizeof(*literal));
    cc_asm_trim(&text);
    if (text.start >= text.end) {
        return;
    }

    if (cc_asm_is_string_literal(text)) {
        literal->bytes = cc_asm_decode_string(text, &literal->length);
        if (literal->bytes != NULL) {
            literal->kind = CC_ASM_LITERAL_STRING;
        }
        return;
    }

    buffer = cc_asm_slice_dup(text);
    if (cc_asm_looks_like_float(buffer)) {
        if (cc_asm_parse_float_text(buffer, &literal->floating)) {
            literal->kind = CC_ASM_LITERAL_FLOAT;
        }
    } else if (cc_asm_parse_integer_text(buffer, &literal->integer, &literal->integer_unsigned)) {
        literal->kind = CC_ASM_LITERAL_INTEGER;
    }
    free(buffer);
}

static CCAsmType cc_asm_type_make(CCAsmScalar scalar, size_t pointer_depth) {
    CCAsmType type;

    type.scalar = scalar;
    type.pointer_depth = pointer_depth;
    return type;
}

static bool cc_asm_type_is_float(CCAsmType type) {
    return type.pointer_depth == 0 && (type.scalar == CC_ASM_SCALAR_F32 || type.scalar == CC_ASM_SCALAR_F64);
}

static bool cc_asm_type_is_aggregate(CCAsmType type) {
    return type.pointer_depth == 0 && type.scalar == CC_ASM_SCALAR_AGGREGATE;
}

static bool cc_asm_type_is_unsigned(CCAsmType type) {
    if (type.pointer_depth > 0) {
        return true;
    }

    switch (type.scalar) {
        case CC_ASM_SCALAR_BOOL:
        case CC_ASM_SCALAR_U8:
        case CC_ASM_SCALAR_U16:
        case CC_ASM_SCALAR_U32:
        case CC_ASM_SCALAR_U64:
            return true;
        default:
            return false;
    }
}

static size_t cc_asm_type_size(CCAsmType type) {
    if (type.pointer_depth > 0) {
        return 8;
    }

    switch (type.scalar) {
        case CC_ASM_SCALAR_BOOL:
        case CC_ASM_SCALAR_I8:
        case CC_ASM_SCALAR_U8:
            return 1;
        case CC_ASM_SCALAR_I16:
        case CC_ASM_SCALAR_U16:
            return 2;
        case CC_ASM_SCALAR_I32:
        case CC_ASM_SCALAR_U32:
        case CC_ASM_SCALAR_F32:
            return 4;
        case CC_ASM_SCALAR_I64:
        case CC_ASM_SCALAR_U64:
        case CC_ASM_SCALAR_F64:
            return 8;
        default:
            return 0;
    }
}

static CCAsmType cc_asm_type_deref(CCAsmType type) {
    if (type.pointer_depth == 0) {
        return cc_asm_type_make(CC_ASM_SCALAR_VOID, 0);
    }

    return cc_asm_type_make(type.scalar, type.pointer_depth - 1);
}

static CCAsmType cc_asm_type_pointer_to(CCAsmType type) {
    return cc_asm_type_make(type.scalar, type.pointer_depth + 1);
}

static CCAsmType cc_asm_type_arithmetic(CCAsmType left, CCAsmType right) {
    bool unsigned_operand;

    if (cc_asm_type_is_float(left) || cc_asm_type_is_float(right)) {
        return cc_asm_type_make(CC_ASM_SCALAR_F64, 0);
    }

    unsigned_operand = cc_asm_type_is_unsigned(left) || cc_asm_type_is_unsigned(right);
    return cc_asm_type_make(unsigned_operand ? CC_ASM_SCALAR_U64 : CC_ASM_SCALAR_I64, 0);
}

static const CCAsmSymbol *cc_asm_lookup_symbol(const CCAsmContext *context, const char *name) {
    size_t index;

    if (name == NULL) {
        return NULL;
    }

    for (index = context->local_count; index > 0; index--) {
        if (strcmp(context->locals[index - 1].name, name) == 0) {
            return &context->locals[index - 1];
        }
    }

    for (index = context->global_count; index > 0; index--) {
        if (strcmp(context->globals[index - 1].name, name) == 0) {
            return &context->globals[index - 1];
        }
    }

    return NULL;
}

static CCAsmSymbol *cc_asm_add_symbol(CCAsmSymbol **items, size_t *count, size_t *capacity, const char *name) {
    CCAsmSymbol *symbol;

    if (*count == *capacity) {
        size_t new_capacity;

        new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        *items = cc_reallocate_or_die(*items, new_capacity * sizeof(**items));
        *capacity = new_capacity;
    }

    symbol = &(*items)[*count];
    memset(symbol, 0, sizeof(*symbol));
    symbol->name = cc_duplicate_string(name);
    symbol->slot = -1;
    symbol->type = cc_asm_type_make(CC_ASM_SCALAR_VOID, 0);
    (*count)++;
    return symbol;
}

static CCAsmSymbol *cc_asm_add_local(CCAsmContext *context, const char *name, CCAsmType type) {
    CCAsmSymbol *symbol;

    symbol = cc_asm_add_symbol(&context->locals, &context->local_count, &context->local_capacity, name);
    symbol->slot = context->slot_count++;
    symbol->type = type;
    return symbol;
}

/* The local table is reused across functions, so the names it owns have to be
   released when the table is rewound rather than only when the context dies. */
static void cc_asm_reset_locals(CCAsmContext *context) {
    size_t index;

    for (index = 0; index < context->local_count; index++) {
        free(context->locals[index].name);
        free(context->locals[index].global_label);
        memset(&context->locals[index], 0, sizeof(context->locals[index]));
    }
    context->local_count = 0;
}

static const CCAsmFunction *cc_asm_lookup_function(const CCAsmContext *context, const char *name) {
    size_t index;

    if (name == NULL) {
        return NULL;
    }

    for (index = context->function_count; index > 0; index--) {
        if (strcmp(context->functions[index - 1].name, name) == 0) {
            return &context->functions[index - 1];
        }
    }

    return NULL;
}

static void cc_asm_register_function(CCAsmContext *context, const char *name, CCAsmType return_type, bool variadic) {
    CCAsmFunction *function;

    if (context->function_count == context->function_capacity) {
        size_t new_capacity;

        new_capacity = context->function_capacity == 0 ? 8 : context->function_capacity * 2;
        context->functions = cc_reallocate_or_die(context->functions, new_capacity * sizeof(*context->functions));
        context->function_capacity = new_capacity;
    }

    function = &context->functions[context->function_count];
    function->name = cc_duplicate_string(name);
    function->return_type = return_type;
    function->variadic = variadic;
    context->function_count++;
}

static const CCAsmTypedef *cc_asm_lookup_typedef(const CCAsmContext *context, const char *name) {
    size_t index;

    for (index = context->typedef_count; index > 0; index--) {
        if (strcmp(context->typedefs[index - 1].name, name) == 0) {
            return &context->typedefs[index - 1];
        }
    }

    return NULL;
}

static void cc_asm_add_typedef(CCAsmContext *context, const char *name, const char *type_text, size_t pointer_depth) {
    CCAsmTypedef *entry;

    if (context->typedef_count == context->typedef_capacity) {
        size_t new_capacity;

        new_capacity = context->typedef_capacity == 0 ? 8 : context->typedef_capacity * 2;
        context->typedefs = cc_reallocate_or_die(context->typedefs, new_capacity * sizeof(*context->typedefs));
        context->typedef_capacity = new_capacity;
    }

    entry = &context->typedefs[context->typedef_count];
    entry->name = cc_duplicate_string(name);
    entry->spelled = cc_duplicate_string(type_text);
    entry->pointer_depth = pointer_depth;
    context->typedef_count++;
}

/* Joins the specifier words of a declaration, dropping storage-class words,
 * mirroring how the textual IR spells types. */
static char *cc_asm_join_specifiers(const CCAstNode *specifiers) {
    CCAsmBuilder builder;
    size_t index;
    bool first;

    memset(&builder, 0, sizeof(builder));
    first = true;

    if (specifiers != NULL) {
        for (index = 0; index < specifiers->child_count; index++) {
            const CCAstNode *child;
            bool skip;

            child = specifiers->children[index];
            if (child->text == NULL) {
                continue;
            }

            skip = strcmp(child->text, "typedef") == 0
                || strcmp(child->text, "static") == 0
                || strcmp(child->text, "extern") == 0
                || strcmp(child->text, "register") == 0
                || strcmp(child->text, "auto") == 0
                || strcmp(child->text, "inline") == 0;
            if (skip) {
                continue;
            }

            if (!first) {
                cc_asm_builder_append_cstring(&builder, " ");
            }
            cc_asm_builder_append_cstring(&builder, child->text);
            first = false;
        }
    }

    if (builder.data == NULL) {
        return cc_duplicate_string("int");
    }

    return cc_asm_builder_take(&builder);
}

static size_t cc_asm_declarator_pointer_depth(const CCAstNode *declarator) {
    size_t depth;
    size_t index;

    if (declarator == NULL) {
        return 0;
    }

    depth = 0;
    for (index = 0; index < declarator->child_count; index++) {
        const CCAstNode *child;

        child = declarator->children[index];
        if (child->kind == CC_AST_POINTER) {
            depth++;
        } else if (child->kind == CC_AST_DECLARATOR) {
            depth += cc_asm_declarator_pointer_depth(child);
        }
    }

    return depth;
}

static void cc_asm_collect_typedefs(CCAsmContext *context) {
    const CCAstNode *unit;
    size_t index;

    unit = context->parse_result->translation_unit;
    if (unit == NULL) {
        return;
    }

    for (index = 0; index < unit->child_count; index++) {
        const CCAstNode *declaration;
        const CCAstNode *specifiers;
        bool is_typedef;
        size_t specifier_index;
        size_t declarator_index;

        declaration = unit->children[index];
        if (declaration->kind != CC_AST_DECLARATION || declaration->child_count == 0) {
            continue;
        }

        specifiers = declaration->children[0];
        is_typedef = false;
        if (specifiers != NULL) {
            for (specifier_index = 0; specifier_index < specifiers->child_count; specifier_index++) {
                if (specifiers->children[specifier_index]->text != NULL
                    && strcmp(specifiers->children[specifier_index]->text, "typedef") == 0) {
                    is_typedef = true;
                    break;
                }
            }
        }
        if (!is_typedef) {
            continue;
        }

        for (declarator_index = 1; declarator_index < declaration->child_count; declarator_index++) {
            const CCAstNode *init_declarator;
            const CCAstNode *declarator;
            char *type_text;

            init_declarator = declaration->children[declarator_index];
            if (init_declarator->kind != CC_AST_INIT_DECLARATOR || init_declarator->child_count == 0) {
                continue;
            }

            declarator = init_declarator->children[0];
            if (declarator->kind != CC_AST_DECLARATOR || declarator->text == NULL) {
                continue;
            }

            type_text = cc_asm_join_specifiers(specifiers);
            cc_asm_add_typedef(context, declarator->text, type_text, cc_asm_declarator_pointer_depth(declarator));
            free(type_text);
        }
    }
}

static CCAsmType cc_asm_type_from_text(CCAsmContext *context, const char *text);

static CCAsmType cc_asm_type_from_identifier(CCAsmContext *context, const char *name, size_t pointer_depth) {
    const CCAsmTypedef *entry;
    CCAsmType type;

    entry = cc_asm_lookup_typedef(context, name);
    if (entry != NULL) {
        type = cc_asm_type_from_text(context, entry->spelled);
        type.pointer_depth += entry->pointer_depth + pointer_depth;
        return type;
    }

    /* Unknown identifiers are unresolved typedef names the IR did not
     * expand. Treating them as 32-bit integers keeps the emitter honest:
     * every load, store and comparison in the tests uses a spelled type. */
    return cc_asm_type_make(CC_ASM_SCALAR_I32, pointer_depth);
}

static CCAsmType cc_asm_type_from_text(CCAsmContext *context, const char *text) {
    char *copy;
    char *cursor;
    char *token;
    char *identifier;
    bool is_unsigned;
    bool has_base;
    size_t pointer_depth;
    CCAsmScalar scalar;

    copy = cc_duplicate_string(text);
    pointer_depth = 0;
    is_unsigned = false;
    has_base = false;
    scalar = CC_ASM_SCALAR_I32;
    identifier = NULL;

    cursor = copy;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '*') {
            if (*cursor == '*') {
                pointer_depth++;
            }
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }

        token = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '*') {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor = '\0';
            cursor++;
        }

        if (strcmp(token, "const") == 0 || strcmp(token, "volatile") == 0 || strcmp(token, "restrict") == 0
            || strcmp(token, "_Atomic") == 0 || strcmp(token, "static") == 0 || strcmp(token, "extern") == 0
            || strcmp(token, "typedef") == 0 || strcmp(token, "register") == 0 || strcmp(token, "inline") == 0
            || strcmp(token, "struct") == 0 || strcmp(token, "union") == 0 || strcmp(token, "enum") == 0) {
            if (strcmp(token, "struct") == 0 || strcmp(token, "union") == 0 || strcmp(token, "enum") == 0) {
                scalar = CC_ASM_SCALAR_AGGREGATE;
                has_base = true;
            }
            continue;
        }
        if (strcmp(token, "unsigned") == 0) {
            is_unsigned = true;
            continue;
        }
        if (strcmp(token, "signed") == 0) {
            continue;
        }
        if (strcmp(token, "void") == 0) {
            scalar = CC_ASM_SCALAR_VOID;
            has_base = true;
            continue;
        }
        if (strcmp(token, "_Bool") == 0 || strcmp(token, "bool") == 0) {
            scalar = CC_ASM_SCALAR_BOOL;
            has_base = true;
            continue;
        }
        if (strcmp(token, "char") == 0) {
            scalar = is_unsigned ? CC_ASM_SCALAR_U8 : CC_ASM_SCALAR_I8;
            has_base = true;
            continue;
        }
        if (strcmp(token, "short") == 0) {
            scalar = is_unsigned ? CC_ASM_SCALAR_U16 : CC_ASM_SCALAR_I16;
            has_base = true;
            continue;
        }
        if (strcmp(token, "int") == 0) {
            if (!has_base) {
                scalar = is_unsigned ? CC_ASM_SCALAR_U32 : CC_ASM_SCALAR_I32;
                has_base = true;
            }
            continue;
        }
        if (strcmp(token, "long") == 0) {
            scalar = is_unsigned ? CC_ASM_SCALAR_U64 : CC_ASM_SCALAR_I64;
            has_base = true;
            continue;
        }
        if (strcmp(token, "float") == 0) {
            scalar = CC_ASM_SCALAR_F32;
            has_base = true;
            continue;
        }
        if (strcmp(token, "double") == 0) {
            scalar = CC_ASM_SCALAR_F64;
            has_base = true;
            continue;
        }

        identifier = token;
    }

    if (!has_base && identifier != NULL) {
        CCAsmType resolved_type;

        resolved_type = cc_asm_type_from_identifier(context, identifier, pointer_depth);
        free(copy);
        return resolved_type;
    }

    free(copy);

    if (!has_base) {
        return cc_asm_type_make(CC_ASM_SCALAR_VOID, pointer_depth);
    }

    return cc_asm_type_make(scalar, pointer_depth);
}

static const char *cc_asm_double_label(CCAsmContext *context, double value) {
    uint64_t bits;
    size_t index;
    CCAsmDouble *entry;

    memcpy(&bits, &value, sizeof(bits));

    for (index = 0; index < context->double_count; index++) {
        if (context->doubles[index].bits == bits) {
            return context->doubles[index].label;
        }
    }

    if (context->double_count == context->double_capacity) {
        size_t new_capacity;

        new_capacity = context->double_capacity == 0 ? 8 : context->double_capacity * 2;
        context->doubles = cc_reallocate_or_die(context->doubles, new_capacity * sizeof(*context->doubles));
        context->double_capacity = new_capacity;
    }

    entry = &context->doubles[context->double_count];
    entry->bits = bits;
    entry->label = cc_format_string(".Lcf%zu", context->double_count);
    context->double_count++;
    return entry->label;
}

static const char *cc_asm_string_label(CCAsmContext *context, const char *bytes, size_t length) {
    size_t index;
    CCAsmString *entry;

    for (index = 0; index < context->string_count; index++) {
        if (context->strings[index].length == length
            && memcmp(context->strings[index].bytes, bytes, length) == 0) {
            return context->strings[index].label;
        }
    }

    if (context->string_count == context->string_capacity) {
        size_t new_capacity;

        new_capacity = context->string_capacity == 0 ? 8 : context->string_capacity * 2;
        context->strings = cc_reallocate_or_die(context->strings, new_capacity * sizeof(*context->strings));
        context->string_capacity = new_capacity;
    }

    entry = &context->strings[context->string_count];
    entry->label = cc_format_string(".Lstr%zu", context->string_count);
    entry->bytes = cc_reallocate_or_die(NULL, length + 1);
    memcpy(entry->bytes, bytes, length);
    entry->bytes[length] = '\0';
    entry->length = length;
    context->string_count++;
    return entry->label;
}

static void cc_asm_set_destination_type(CCAsmSymbol *symbol, CCAsmType type) {
    if (!symbol->declared) {
        symbol->type = type;
    }
}

static char *cc_asm_symbol_memory(const CCAsmSymbol *symbol) {
    if (symbol->slot >= 0) {
        return cc_format_string("%d(%%rbp)", -(symbol->slot + 1) * 8);
    }

    return cc_format_string("%s(%%rip)", symbol->global_label);
}

static void cc_asm_load_integer_into(CCAsmContext *context, CCAsmSlice operand, int register_index) {
    CCAsmLiteral literal;
    const CCAsmSymbol *symbol;
    char *memory;
    char *name;
    CCAsmType type;
    size_t size;

    cc_asm_parse_literal(operand, &literal);
    if (literal.kind == CC_ASM_LITERAL_INTEGER) {
        cc_asm_emit_line(&context->output, "  movabsq $%lld, %s", literal.integer, CC_ASM_INT_REGISTERS[register_index]);
        cc_asm_literal_free(&literal);
        return;
    }
    cc_asm_literal_free(&literal);

    name = cc_asm_slice_dup(operand);
    symbol = cc_asm_lookup_symbol(context, name);
    if (symbol == NULL) {
        cc_asm_error(context, "x86-64 backend cannot resolve value '%s'", name);
        free(name);
        return;
    }
    free(name);

    type = symbol->type;
    size = cc_asm_type_size(type);
    memory = cc_asm_symbol_memory(symbol);

    if (type.pointer_depth > 0) {
        cc_asm_emit_line(&context->output, "  movq %s, %s", memory, CC_ASM_INT_REGISTERS[register_index]);
    } else {
        switch (size) {
            case 1:
                if (cc_asm_type_is_unsigned(type) || type.scalar == CC_ASM_SCALAR_BOOL) {
                    cc_asm_emit_line(&context->output, "  movzbq %s, %s", memory, CC_ASM_INT_REGISTERS[register_index]);
                } else {
                    cc_asm_emit_line(&context->output, "  movsbq %s, %s", memory, CC_ASM_INT_REGISTERS[register_index]);
                }
                break;
            case 2:
                if (cc_asm_type_is_unsigned(type)) {
                    cc_asm_emit_line(&context->output, "  movzwq %s, %s", memory, CC_ASM_INT_REGISTERS[register_index]);
                } else {
                    cc_asm_emit_line(&context->output, "  movswq %s, %s", memory, CC_ASM_INT_REGISTERS[register_index]);
                }
                break;
            case 4:
                if (cc_asm_type_is_unsigned(type)) {
                    cc_asm_emit_line(&context->output, "  movl %s, %s", memory, CC_ASM_INT_32[register_index]);
                } else {
                    cc_asm_emit_line(&context->output, "  movslq %s, %s", memory, CC_ASM_INT_REGISTERS[register_index]);
                }
                break;
            default:
                cc_asm_emit_line(&context->output, "  movq %s, %s", memory, CC_ASM_INT_REGISTERS[register_index]);
                break;
        }
    }

    free(memory);
}

static void cc_asm_load_float_into(CCAsmContext *context, CCAsmSlice operand, int xmm_index) {
    CCAsmLiteral literal;
    const CCAsmSymbol *symbol;
    char *memory;
    char *name;

    cc_asm_parse_literal(operand, &literal);
    if (literal.kind == CC_ASM_LITERAL_FLOAT) {
        const char *label;

        label = cc_asm_double_label(context, literal.floating);
        cc_asm_emit_line(&context->output, "  movsd %s(%%rip), %%xmm%d", label, xmm_index);
        cc_asm_literal_free(&literal);
        return;
    }
    if (literal.kind == CC_ASM_LITERAL_INTEGER) {
        cc_asm_emit_line(&context->output, "  movabsq $%lld, %%rax", literal.integer);
        cc_asm_emit_line(&context->output, "  cvtsi2sdq %%rax, %%xmm%d", xmm_index);
        cc_asm_literal_free(&literal);
        return;
    }
    cc_asm_literal_free(&literal);

    name = cc_asm_slice_dup(operand);
    symbol = cc_asm_lookup_symbol(context, name);
    if (symbol == NULL) {
        cc_asm_error(context, "x86-64 backend cannot resolve value '%s'", name);
        free(name);
        return;
    }
    free(name);

    memory = cc_asm_symbol_memory(symbol);
    cc_asm_emit_line(&context->output, "  movsd %s, %%xmm%d", memory, xmm_index);
    free(memory);
}

static void cc_asm_store_integer(CCAsmContext *context, const CCAsmSymbol *symbol) {
    char *memory;
    size_t size;

    size = cc_asm_type_size(symbol->type);
    memory = cc_asm_symbol_memory(symbol);

    switch (size) {
        case 1:
            cc_asm_emit_line(&context->output, "  movb %%al, %s", memory);
            break;
        case 2:
            cc_asm_emit_line(&context->output, "  movw %%ax, %s", memory);
            break;
        case 4:
            cc_asm_emit_line(&context->output, "  movl %%eax, %s", memory);
            break;
        default:
            cc_asm_emit_line(&context->output, "  movq %%rax, %s", memory);
            break;
    }

    free(memory);
}

static void cc_asm_store_float(CCAsmContext *context, const CCAsmSymbol *symbol) {
    char *memory;

    memory = cc_asm_symbol_memory(symbol);
    cc_asm_emit_line(&context->output, "  movsd %%xmm0, %s", memory);
    free(memory);
}

static CCAsmType cc_asm_operand_type(CCAsmContext *context, CCAsmSlice operand) {
    CCAsmLiteral literal;
    const CCAsmSymbol *symbol;
    char *name;
    CCAsmType type;

    cc_asm_parse_literal(operand, &literal);
    if (literal.kind == CC_ASM_LITERAL_STRING) {
        cc_asm_literal_free(&literal);
        return cc_asm_type_make(CC_ASM_SCALAR_I8, 1);
    }
    if (literal.kind == CC_ASM_LITERAL_FLOAT) {
        cc_asm_literal_free(&literal);
        return cc_asm_type_make(CC_ASM_SCALAR_F64, 0);
    }
    if (literal.kind == CC_ASM_LITERAL_INTEGER) {
        CCAsmType literal_type;

        literal_type = cc_asm_type_make(literal.integer_unsigned ? CC_ASM_SCALAR_U64 : CC_ASM_SCALAR_I64, 0);
        cc_asm_literal_free(&literal);
        return literal_type;
    }
    cc_asm_literal_free(&literal);

    name = cc_asm_slice_dup(operand);
    symbol = cc_asm_lookup_symbol(context, name);
    free(name);

    if (symbol == NULL) {
        return cc_asm_type_make(CC_ASM_SCALAR_I64, 0);
    }

    type = symbol->type;
    return type;
}

static CCAsmSymbol *cc_asm_require_local(CCAsmContext *context, const char *name) {
    size_t index;

    for (index = context->local_count; index > 0; index--) {
        if (strcmp(context->locals[index - 1].name, name) == 0) {
            return &context->locals[index - 1];
        }
    }

    return cc_asm_add_local(context, name, cc_asm_type_make(CC_ASM_SCALAR_I64, 0));
}

/* Places: a bare symbol, a subscript of a pointer symbol, or a dereference of
 * a symbol. Leaves the effective address in %r10. */
typedef struct {
    char *base;
    char *index;
    bool dereference;
} CCAsmPlace;

static void cc_asm_place_free(CCAsmPlace *place) {
    free(place->base);
    free(place->index);
    memset(place, 0, sizeof(*place));
}

static bool cc_asm_parse_place(CCAsmContext *context, CCAsmSlice text, CCAsmPlace *place) {
    CCAsmSlice trimmed;
    char *text_copy;
    char *bracket;

    memset(place, 0, sizeof(*place));
    trimmed = text;
    cc_asm_trim(&trimmed);
    text_copy = cc_asm_slice_dup(trimmed);

    if (text_copy[0] == '*') {
        place->dereference = true;
        place->base = cc_duplicate_string(text_copy + 1);
        free(text_copy);
        return true;
    }

    bracket = strchr(text_copy, '[');
    if (bracket != NULL) {
        char *closing;

        closing = strrchr(text_copy, ']');
        if (closing == NULL || closing < bracket) {
            free(text_copy);
            cc_asm_error(context, "malformed subscript place in generated IR");
            return false;
        }
        *bracket = '\0';
        *closing = '\0';
        place->base = cc_duplicate_string(text_copy);
        place->index = cc_duplicate_string(bracket + 1);
        free(text_copy);
        return true;
    }

    if (cc_asm_is_place_text(text_copy)) {
        free(text_copy);
        cc_asm_error(context, "x86-64 backend does not support struct member access yet");
        return false;
    }

    place->base = text_copy;
    return true;
}

/* Type of the object a place names, used for stores and for addr. */
static CCAsmType cc_asm_place_object_type(CCAsmContext *context, const CCAsmPlace *place) {
    const CCAsmSymbol *symbol;

    symbol = cc_asm_lookup_symbol(context, place->base);
    if (symbol == NULL) {
        return cc_asm_type_make(CC_ASM_SCALAR_I64, 0);
    }

    if (place->index != NULL || place->dereference) {
        return cc_asm_type_deref(symbol->type);
    }

    return symbol->type;
}

/* Type loaded by the IR 'load' instruction. The IR names an address
 * expression there, so a bare symbol means "the pointee of this value". */
static CCAsmType cc_asm_place_load_type(CCAsmContext *context, const CCAsmPlace *place) {
    const CCAsmSymbol *symbol;

    symbol = cc_asm_lookup_symbol(context, place->base);
    if (symbol == NULL) {
        return cc_asm_type_make(CC_ASM_SCALAR_I64, 0);
    }

    return cc_asm_type_deref(symbol->type);
}

static void cc_asm_place_address(CCAsmContext *context, const CCAsmPlace *place, bool address_is_value) {
    const CCAsmSymbol *symbol;
    char *memory;
    size_t size;

    symbol = cc_asm_lookup_symbol(context, place->base);
    if (symbol == NULL) {
        cc_asm_error(context, "x86-64 backend cannot resolve place '%s'", place->base);
        return;
    }

    if (place->dereference) {
        memory = cc_asm_symbol_memory(symbol);
        cc_asm_emit_line(&context->output, "  movq %s, %%r10", memory);
        free(memory);
        return;
    }

    if (place->index != NULL) {
        size = cc_asm_type_size(cc_asm_type_deref(symbol->type));
        if (size == 0) {
            size = 8;
        }

        memory = cc_asm_symbol_memory(symbol);
        cc_asm_emit_line(&context->output, "  movq %s, %%r10", memory);
        free(memory);

        {
            CCAsmSlice index_slice;
            CCAsmLiteral literal;

            index_slice.start = place->index;
            index_slice.end = place->index + strlen(place->index);
            cc_asm_trim(&index_slice);
            cc_asm_parse_literal(index_slice, &literal);
            if (literal.kind == CC_ASM_LITERAL_INTEGER) {
                cc_asm_emit_line(&context->output, "  movabsq $%lld, %%r11", literal.integer);
                cc_asm_literal_free(&literal);
            } else {
                cc_asm_literal_free(&literal);
                cc_asm_load_integer_into(context, index_slice, 6);
            }
        }

        cc_asm_emit_line(&context->output, "  imulq $%zu, %%r11, %%r11", size);
        cc_asm_emit_line(&context->output, "  addq %%r11, %%r10");
        return;
    }

    memory = cc_asm_symbol_memory(symbol);
    if (address_is_value) {
        cc_asm_emit_line(&context->output, "  movq %s, %%r10", memory);
    } else {
        cc_asm_emit_line(&context->output, "  leaq %s, %%r10", memory);
    }
    free(memory);
}

static void cc_asm_load_place(CCAsmContext *context, const CCAsmPlace *place, CCAsmType type) {
    size_t size;

    cc_asm_place_address(context, place, true);
    if (context->failed) {
        return;
    }

    if (cc_asm_type_is_float(type)) {
        cc_asm_emit_line(&context->output, "  movsd (%%r10), %%xmm0");
        return;
    }

    size = cc_asm_type_size(type);
    switch (size) {
        case 1:
            if (cc_asm_type_is_unsigned(type) || type.scalar == CC_ASM_SCALAR_BOOL) {
                cc_asm_emit_line(&context->output, "  movzbq (%%r10), %%rax");
            } else {
                cc_asm_emit_line(&context->output, "  movsbq (%%r10), %%rax");
            }
            break;
        case 2:
            if (cc_asm_type_is_unsigned(type)) {
                cc_asm_emit_line(&context->output, "  movzwq (%%r10), %%rax");
            } else {
                cc_asm_emit_line(&context->output, "  movswq (%%r10), %%rax");
            }
            break;
        case 4:
            if (cc_asm_type_is_unsigned(type)) {
                cc_asm_emit_line(&context->output, "  movl (%%r10), %%eax");
            } else {
                cc_asm_emit_line(&context->output, "  movslq (%%r10), %%rax");
            }
            break;
        default:
            cc_asm_emit_line(&context->output, "  movq (%%r10), %%rax");
            break;
    }
}

static void cc_asm_store_place(CCAsmContext *context, const CCAsmPlace *place, CCAsmType type, bool from_float) {
    size_t size;

    cc_asm_place_address(context, place, false);
    if (context->failed) {
        return;
    }

    if (from_float || cc_asm_type_is_float(type)) {
        cc_asm_emit_line(&context->output, "  movsd %%xmm0, (%%r10)");
        return;
    }

    size = cc_asm_type_size(type);
    switch (size) {
        case 1:
            cc_asm_emit_line(&context->output, "  movb %%al, (%%r10)");
            break;
        case 2:
            cc_asm_emit_line(&context->output, "  movw %%ax, (%%r10)");
            break;
        case 4:
            cc_asm_emit_line(&context->output, "  movl %%eax, (%%r10)");
            break;
        default:
            cc_asm_emit_line(&context->output, "  movq %%rax, (%%r10)");
            break;
    }
}

static char *cc_asm_label_name(CCAsmContext *context, CCAsmSlice label) {
    char *name;
    char *result;
    size_t index;

    name = cc_asm_slice_dup(label);
    for (index = 0; name[index] != '\0'; index++) {
        char ch;

        ch = name[index];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.')) {
            name[index] = '_';
        }
    }

    result = cc_format_string(".L%s.%s", context->function_name == NULL ? "unit" : context->function_name, name);
    free(name);
    return result;
}

static bool cc_asm_is_binary_opcode(CCAsmSlice word) {
    static const char *const opcodes[] = {
        "add", "sub", "mul", "div", "mod", "shl", "shr",
        "bitand", "bitor", "bitxor", "and", "or",
        "eq", "ne", "lt", "le", "gt", "ge"
    };
    size_t index;

    for (index = 0; index < sizeof(opcodes) / sizeof(opcodes[0]); index++) {
        if (cc_asm_slice_equals(word, opcodes[index])) {
            return true;
        }
    }

    return false;
}

static void cc_asm_emit_binary(CCAsmContext *context, CCAsmSymbol *destination, CCAsmSlice opcode, CCAsmSlice left, CCAsmSlice right) {
    CCAsmType left_type;
    CCAsmType right_type;
    CCAsmType result_type;
    bool is_float;
    char *opcode_text;

    left_type = cc_asm_operand_type(context, left);
    right_type = cc_asm_operand_type(context, right);
    result_type = cc_asm_type_arithmetic(left_type, right_type);
    is_float = cc_asm_type_is_float(result_type);
    opcode_text = cc_asm_slice_dup(opcode);

    if (strcmp(opcode_text, "eq") == 0 || strcmp(opcode_text, "ne") == 0
        || strcmp(opcode_text, "lt") == 0 || strcmp(opcode_text, "le") == 0
        || strcmp(opcode_text, "gt") == 0 || strcmp(opcode_text, "ge") == 0) {
        bool is_unsigned;

        is_unsigned = cc_asm_type_is_unsigned(left_type) || cc_asm_type_is_unsigned(right_type);

        if (is_float) {
            const char *set_instruction;

            cc_asm_load_float_into(context, left, 0);
            cc_asm_load_float_into(context, right, 1);
            cc_asm_emit_line(&context->output, "  ucomisd %%xmm1, %%xmm0");

            if (strcmp(opcode_text, "eq") == 0) {
                cc_asm_emit_line(&context->output, "  sete %%al");
                cc_asm_emit_line(&context->output, "  setnp %%cl");
                cc_asm_emit_line(&context->output, "  andb %%cl, %%al");
            } else if (strcmp(opcode_text, "ne") == 0) {
                cc_asm_emit_line(&context->output, "  setne %%al");
                cc_asm_emit_line(&context->output, "  setp %%cl");
                cc_asm_emit_line(&context->output, "  orb %%cl, %%al");
            } else {
                if (strcmp(opcode_text, "lt") == 0) {
                    set_instruction = "setb";
                } else if (strcmp(opcode_text, "le") == 0) {
                    set_instruction = "setbe";
                } else if (strcmp(opcode_text, "gt") == 0) {
                    set_instruction = "seta";
                } else {
                    set_instruction = "setae";
                }
                cc_asm_emit_line(&context->output, "  %s %%al", set_instruction);
            }
            cc_asm_emit_line(&context->output, "  movzbq %%al, %%rax");
        } else {
            const char *set_instruction;

            cc_asm_load_integer_into(context, left, 0);
            cc_asm_load_integer_into(context, right, 1);
            cc_asm_emit_line(&context->output, "  cmpq %%rcx, %%rax");

            if (strcmp(opcode_text, "eq") == 0) {
                set_instruction = "sete";
            } else if (strcmp(opcode_text, "ne") == 0) {
                set_instruction = "setne";
            } else if (strcmp(opcode_text, "lt") == 0) {
                set_instruction = is_unsigned ? "setb" : "setl";
            } else if (strcmp(opcode_text, "le") == 0) {
                set_instruction = is_unsigned ? "setbe" : "setle";
            } else if (strcmp(opcode_text, "gt") == 0) {
                set_instruction = is_unsigned ? "seta" : "setg";
            } else {
                set_instruction = is_unsigned ? "setae" : "setge";
            }
            cc_asm_emit_line(&context->output, "  %s %%al", set_instruction);
            cc_asm_emit_line(&context->output, "  movzbq %%al, %%rax");
        }

        cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_I32, 0));
        cc_asm_store_integer(context, destination);
        free(opcode_text);
        return;
    }

    if (is_float) {
        cc_asm_load_float_into(context, left, 0);
        cc_asm_load_float_into(context, right, 1);

        if (strcmp(opcode_text, "add") == 0) {
            cc_asm_emit_line(&context->output, "  addsd %%xmm1, %%xmm0");
        } else if (strcmp(opcode_text, "sub") == 0) {
            cc_asm_emit_line(&context->output, "  subsd %%xmm1, %%xmm0");
        } else if (strcmp(opcode_text, "mul") == 0) {
            cc_asm_emit_line(&context->output, "  mulsd %%xmm1, %%xmm0");
        } else if (strcmp(opcode_text, "div") == 0) {
            cc_asm_emit_line(&context->output, "  divsd %%xmm1, %%xmm0");
        } else {
            cc_asm_error(context, "x86-64 backend does not support floating point '%s'", opcode_text);
            free(opcode_text);
            return;
        }

        cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_F64, 0));
        cc_asm_store_float(context, destination);
        free(opcode_text);
        return;
    }

    cc_asm_load_integer_into(context, left, 0);
    cc_asm_load_integer_into(context, right, 1);

    if (strcmp(opcode_text, "add") == 0) {
        cc_asm_emit_line(&context->output, "  addq %%rcx, %%rax");
    } else if (strcmp(opcode_text, "sub") == 0) {
        cc_asm_emit_line(&context->output, "  subq %%rcx, %%rax");
    } else if (strcmp(opcode_text, "mul") == 0) {
        cc_asm_emit_line(&context->output, "  imulq %%rcx, %%rax");
    } else if (strcmp(opcode_text, "div") == 0) {
        if (cc_asm_type_is_unsigned(left_type) || cc_asm_type_is_unsigned(right_type)) {
            cc_asm_emit_line(&context->output, "  xorl %%edx, %%edx");
            cc_asm_emit_line(&context->output, "  divq %%rcx");
        } else {
            cc_asm_emit_line(&context->output, "  cqto");
            cc_asm_emit_line(&context->output, "  idivq %%rcx");
        }
    } else if (strcmp(opcode_text, "mod") == 0) {
        if (cc_asm_type_is_unsigned(left_type) || cc_asm_type_is_unsigned(right_type)) {
            cc_asm_emit_line(&context->output, "  xorl %%edx, %%edx");
            cc_asm_emit_line(&context->output, "  divq %%rcx");
        } else {
            cc_asm_emit_line(&context->output, "  cqto");
            cc_asm_emit_line(&context->output, "  idivq %%rcx");
        }
        cc_asm_emit_line(&context->output, "  movq %%rdx, %%rax");
    } else if (strcmp(opcode_text, "shl") == 0) {
        cc_asm_emit_line(&context->output, "  shlq %%cl, %%rax");
    } else if (strcmp(opcode_text, "shr") == 0) {
        if (cc_asm_type_is_unsigned(left_type)) {
            cc_asm_emit_line(&context->output, "  shrq %%cl, %%rax");
        } else {
            cc_asm_emit_line(&context->output, "  sarq %%cl, %%rax");
        }
    } else if (strcmp(opcode_text, "bitand") == 0) {
        cc_asm_emit_line(&context->output, "  andq %%rcx, %%rax");
    } else if (strcmp(opcode_text, "bitor") == 0) {
        cc_asm_emit_line(&context->output, "  orq %%rcx, %%rax");
    } else if (strcmp(opcode_text, "bitxor") == 0) {
        cc_asm_emit_line(&context->output, "  xorq %%rcx, %%rax");
    } else if (strcmp(opcode_text, "and") == 0 || strcmp(opcode_text, "or") == 0) {
        cc_asm_emit_line(&context->output, "  testq %%rax, %%rax");
        cc_asm_emit_line(&context->output, "  setne %%al");
        cc_asm_emit_line(&context->output, "  movzbq %%al, %%rax");
        cc_asm_emit_line(&context->output, "  testq %%rcx, %%rcx");
        cc_asm_emit_line(&context->output, "  setne %%cl");
        cc_asm_emit_line(&context->output, "  movzbq %%cl, %%rcx");
        if (strcmp(opcode_text, "and") == 0) {
            cc_asm_emit_line(&context->output, "  andq %%rcx, %%rax");
        } else {
            cc_asm_emit_line(&context->output, "  orq %%rcx, %%rax");
        }
        cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_I32, 0));
        cc_asm_store_integer(context, destination);
        free(opcode_text);
        return;
    } else {
        cc_asm_error(context, "x86-64 backend does not support binary operator '%s'", opcode_text);
        free(opcode_text);
        return;
    }

    cc_asm_set_destination_type(destination, result_type);
    cc_asm_store_integer(context, destination);
    free(opcode_text);
}

static void cc_asm_emit_unary(CCAsmContext *context, CCAsmSymbol *destination, CCAsmSlice opcode, CCAsmSlice operand) {
    char *opcode_text;
    CCAsmType operand_type;

    opcode_text = cc_asm_slice_dup(opcode);
    operand_type = cc_asm_operand_type(context, operand);

    if (strcmp(opcode_text, "identity") == 0) {
        if (cc_asm_type_is_float(operand_type)) {
            cc_asm_load_float_into(context, operand, 0);
            cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_F64, 0));
            cc_asm_store_float(context, destination);
        } else {
            cc_asm_load_integer_into(context, operand, 0);
            cc_asm_set_destination_type(destination, operand_type);
            cc_asm_store_integer(context, destination);
        }
        free(opcode_text);
        return;
    }

    if (strcmp(opcode_text, "neg") == 0 && cc_asm_type_is_float(operand_type)) {
        cc_asm_load_float_into(context, operand, 0);
        cc_asm_emit_line(&context->output, "  movq %%xmm0, %%rax");
        cc_asm_emit_line(&context->output, "  movabsq $-9223372036854775808, %%rcx");
        cc_asm_emit_line(&context->output, "  xorq %%rcx, %%rax");
        cc_asm_emit_line(&context->output, "  movq %%rax, %%xmm0");
        cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_F64, 0));
        cc_asm_store_float(context, destination);
        free(opcode_text);
        return;
    }

    if (strcmp(opcode_text, "not") == 0 && cc_asm_type_is_float(operand_type)) {
        cc_asm_load_float_into(context, operand, 0);
        cc_asm_emit_line(&context->output, "  xorpd %%xmm1, %%xmm1");
        cc_asm_emit_line(&context->output, "  ucomisd %%xmm1, %%xmm0");
        cc_asm_emit_line(&context->output, "  setne %%al");
        cc_asm_emit_line(&context->output, "  setp %%cl");
        cc_asm_emit_line(&context->output, "  orb %%cl, %%al");
        cc_asm_emit_line(&context->output, "  movzbq %%al, %%rax");
        cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_I32, 0));
        cc_asm_store_integer(context, destination);
        free(opcode_text);
        return;
    }

    cc_asm_load_integer_into(context, operand, 0);

    if (strcmp(opcode_text, "neg") == 0) {
        cc_asm_emit_line(&context->output, "  negq %%rax");
    } else if (strcmp(opcode_text, "bitnot") == 0) {
        cc_asm_emit_line(&context->output, "  notq %%rax");
    } else if (strcmp(opcode_text, "not") == 0) {
        cc_asm_emit_line(&context->output, "  testq %%rax, %%rax");
        cc_asm_emit_line(&context->output, "  sete %%al");
        cc_asm_emit_line(&context->output, "  movzbq %%al, %%rax");
        cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_I32, 0));
        cc_asm_store_integer(context, destination);
        free(opcode_text);
        return;
    } else {
        cc_asm_error(context, "x86-64 backend does not support unary operator '%s'", opcode_text);
        free(opcode_text);
        return;
    }

    cc_asm_set_destination_type(destination, operand_type);
    cc_asm_store_integer(context, destination);
    free(opcode_text);
}

static void cc_asm_emit_cast(CCAsmContext *context, CCAsmSymbol *destination, CCAsmSlice type_slice, CCAsmSlice operand) {
    char *type_text;
    CCAsmType target_type;
    CCAsmType operand_type;
    size_t size;

    type_text = cc_asm_slice_dup(type_slice);
    target_type = cc_asm_type_from_text(context, type_text);
    free(type_text);

    if (target_type.pointer_depth == 0 && target_type.scalar == CC_ASM_SCALAR_F32) {
        cc_asm_error(context, "x86-64 backend does not support single precision floats yet");
        return;
    }
    if (cc_asm_type_is_aggregate(target_type)) {
        cc_asm_error(context, "x86-64 backend does not support struct values yet");
        return;
    }

    operand_type = cc_asm_operand_type(context, operand);

    if (cc_asm_type_is_float(target_type)) {
        if (cc_asm_type_is_float(operand_type)) {
            cc_asm_load_float_into(context, operand, 0);
        } else {
            cc_asm_load_integer_into(context, operand, 0);
            cc_asm_emit_line(&context->output, "  cvtsi2sdq %%rax, %%xmm0");
        }
        cc_asm_set_destination_type(destination, target_type);
        cc_asm_store_float(context, destination);
        return;
    }

    if (cc_asm_type_is_float(operand_type)) {
        cc_asm_load_float_into(context, operand, 0);
        cc_asm_emit_line(&context->output, "  cvttsd2siq %%xmm0, %%rax");
    } else {
        cc_asm_load_integer_into(context, operand, 0);
    }

    size = cc_asm_type_size(target_type);
    if (target_type.pointer_depth == 0) {
        switch (size) {
            case 1:
                if (cc_asm_type_is_unsigned(target_type) || target_type.scalar == CC_ASM_SCALAR_BOOL) {
                    cc_asm_emit_line(&context->output, "  movzbq %%al, %%rax");
                } else {
                    cc_asm_emit_line(&context->output, "  movsbq %%al, %%rax");
                }
                break;
            case 2:
                if (cc_asm_type_is_unsigned(target_type)) {
                    cc_asm_emit_line(&context->output, "  movzwq %%ax, %%rax");
                } else {
                    cc_asm_emit_line(&context->output, "  movswq %%ax, %%rax");
                }
                break;
            case 4:
                if (cc_asm_type_is_unsigned(target_type)) {
                    cc_asm_emit_line(&context->output, "  movl %%eax, %%eax");
                } else {
                    cc_asm_emit_line(&context->output, "  movslq %%eax, %%rax");
                }
                break;
            default:
                break;
        }
    }

    cc_asm_set_destination_type(destination, target_type);
    cc_asm_store_integer(context, destination);
}

static void cc_asm_emit_call(CCAsmContext *context, CCAsmSymbol *destination, CCAsmSlice call_slice) {
    const char *open;
    const char *close;
    CCAsmSlice callee_slice;
    CCAsmSlice arguments[CC_ASM_MAX_CALL_REGISTERS];
    size_t argument_count;
    char *callee;
    const CCAsmFunction *function;
    int integer_index;
    int float_index;
    size_t index;

    open = call_slice.start;
    while (open < call_slice.end && *open != '(') {
        open++;
    }
    if (open >= call_slice.end) {
        cc_asm_error(context, "malformed call in generated IR");
        return;
    }

    close = call_slice.end;
    while (close > open && close[-1] != ')') {
        close--;
    }
    if (close <= open) {
        cc_asm_error(context, "malformed call in generated IR");
        return;
    }

    callee_slice.start = call_slice.start;
    callee_slice.end = open;
    cc_asm_trim(&callee_slice);
    callee = cc_asm_slice_dup(callee_slice);

    {
        CCAsmSlice argument_slice;

        argument_slice.start = open + 1;
        argument_slice.end = close - 1;
        argument_count = 0;
        if (argument_slice.start < argument_slice.end) {
            argument_count = cc_asm_split_list(argument_slice, arguments, CC_ASM_MAX_CALL_REGISTERS);
        }
    }

    function = cc_asm_lookup_function(context, callee);

    integer_index = 0;
    float_index = 0;
    for (index = 0; index < argument_count; index++) {
        CCAsmType type;

        type = cc_asm_operand_type(context, arguments[index]);
        if (cc_asm_type_is_float(type)) {
            if (float_index >= 8) {
                cc_asm_error(context, "x86-64 backend supports at most 8 floating point call arguments");
                free(callee);
                return;
            }
            cc_asm_load_float_into(context, arguments[index], float_index);
            float_index++;
        } else {
            if (integer_index >= 6) {
                cc_asm_error(context, "x86-64 backend supports at most 6 integer call arguments");
                free(callee);
                return;
            }
            {
                int target;
                char *memory;
                const CCAsmSymbol *symbol;
                CCAsmLiteral literal;
                char *name;

                cc_asm_parse_literal(arguments[index], &literal);
                if (literal.kind == CC_ASM_LITERAL_INTEGER) {
                    cc_asm_emit_line(&context->output, "  movabsq $%lld, %s", literal.integer, CC_ASM_INT_ARGUMENTS[integer_index]);
                    cc_asm_literal_free(&literal);
                    integer_index++;
                    continue;
                }
                if (literal.kind == CC_ASM_LITERAL_STRING) {
                    const char *label;

                    label = cc_asm_string_label(context, literal.bytes, literal.length);
                    cc_asm_emit_line(&context->output, "  leaq %s(%%rip), %s", label, CC_ASM_INT_ARGUMENTS[integer_index]);
                    cc_asm_literal_free(&literal);
                    integer_index++;
                    continue;
                }
                cc_asm_literal_free(&literal);

                name = cc_asm_slice_dup(arguments[index]);
                symbol = cc_asm_lookup_symbol(context, name);
                free(name);
                if (symbol == NULL) {
                    cc_asm_error(context, "x86-64 backend cannot resolve call argument");
                    free(callee);
                    return;
                }

                target = integer_index;
                memory = cc_asm_symbol_memory(symbol);
                if (symbol->type.pointer_depth > 0 || cc_asm_type_size(symbol->type) == 8) {
                    cc_asm_emit_line(&context->output, "  movq %s, %s", memory, CC_ASM_INT_ARGUMENTS[target]);
                } else if (cc_asm_type_size(symbol->type) == 4) {
                    if (cc_asm_type_is_unsigned(symbol->type)) {
                        cc_asm_emit_line(&context->output, "  movl %s, %s", memory, cc_asm_int_argument_32(target));
                    } else {
                        cc_asm_emit_line(&context->output, "  movslq %s, %s", memory, CC_ASM_INT_ARGUMENTS[target]);
                    }
                } else if (cc_asm_type_size(symbol->type) == 2) {
                    if (cc_asm_type_is_unsigned(symbol->type)) {
                        cc_asm_emit_line(&context->output, "  movzwq %s, %s", memory, CC_ASM_INT_ARGUMENTS[target]);
                    } else {
                        cc_asm_emit_line(&context->output, "  movswq %s, %s", memory, CC_ASM_INT_ARGUMENTS[target]);
                    }
                } else {
                    if (cc_asm_type_is_unsigned(symbol->type)) {
                        cc_asm_emit_line(&context->output, "  movzbq %s, %s", memory, CC_ASM_INT_ARGUMENTS[target]);
                    } else {
                        cc_asm_emit_line(&context->output, "  movsbq %s, %s", memory, CC_ASM_INT_ARGUMENTS[target]);
                    }
                }
                free(memory);
                integer_index++;
            }
        }
    }

    cc_asm_emit_line(&context->output, "  movb $%d, %%al", float_index);
    cc_asm_emit_line(&context->output, "  call %s%s", CC_ASM_GLOBAL_PREFIX, callee);

    if (function != NULL) {
        cc_asm_set_destination_type(destination, function->return_type);
    } else {
        cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_I32, 0));
    }

    if (cc_asm_type_is_float(destination->type)) {
        cc_asm_store_float(context, destination);
    } else {
        cc_asm_store_integer(context, destination);
    }

    free(callee);
}

/* True when the right hand side is a literal or a single named value, so it
 * can be loaded straight into the value register. */
static bool cc_asm_is_simple_value(CCAsmSlice rhs) {
    CCAsmSlice word;
    const char *cursor;
    const char *end;

    cursor = rhs.start;
    end = rhs.end;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }
    word.start = cursor;
    while (cursor < end && *cursor != ' ' && *cursor != '\t') {
        cursor++;
    }
    word.end = cursor;

    if (cc_asm_slice_equals(word, "load") || cc_asm_slice_equals(word, "addr")
        || cc_asm_slice_equals(word, "cast") || cc_asm_slice_equals(word, "call")
        || cc_asm_slice_equals(word, "neg") || cc_asm_slice_equals(word, "bitnot")
        || cc_asm_slice_equals(word, "not") || cc_asm_slice_equals(word, "identity")
        || cc_asm_slice_equals(word, "size_of") || cc_asm_slice_equals(word, "kw_sizeof")
        || cc_asm_slice_equals(word, "alignof") || cc_asm_slice_equals(word, "kw__Alignof")) {
        return false;
    }

    if (cc_asm_is_binary_opcode(word) && memchr(cursor, ',', (size_t)(end - cursor)) != NULL) {
        return false;
    }

    return true;
}

static void cc_asm_emit_assignment(CCAsmContext *context, CCAsmSlice lhs, CCAsmSlice rhs) {
    char *lhs_text;
    CCAsmSymbol *destination;
    CCAsmSlice word;
    CCAsmSlice rest;
    CCAsmSlice operands[2];
    size_t operand_count;
    const char *cursor;
    const char *end;

    lhs_text = cc_asm_slice_dup(lhs);

    if (cc_asm_is_place_text(lhs_text)) {
        CCAsmPlace place;
        CCAsmType place_type;
        char *rhs_text;
        CCAsmLiteral literal;
        CCAsmSlice operand;

        free(lhs_text);
        if (!cc_asm_parse_place(context, lhs, &place)) {
            return;
        }
        place_type = cc_asm_place_object_type(context, &place);

        rhs_text = cc_asm_slice_dup(rhs);
        operand.start = rhs_text;
        operand.end = rhs_text + strlen(rhs_text);
        cc_asm_parse_literal(operand, &literal);
        if (literal.kind == CC_ASM_LITERAL_STRING) {
            const char *label;

            label = cc_asm_string_label(context, literal.bytes, literal.length);
            cc_asm_emit_line(&context->output, "  leaq %s(%%rip), %%rax", label);
            cc_asm_store_place(context, &place, place_type, false);
            cc_asm_literal_free(&literal);
            free(rhs_text);
            cc_asm_place_free(&place);
            return;
        }
        cc_asm_literal_free(&literal);
        free(rhs_text);

        if (!cc_asm_is_simple_value(rhs)) {
            char *scratch_name;
            CCAsmSlice scratch_slice;
            const CCAsmSymbol *scratch;

            if (context->scratch_used >= context->scratch_count) {
                cc_asm_error(context, "x86-64 backend ran out of scratch slots");
                cc_asm_place_free(&place);
                return;
            }
            scratch_name = cc_format_string("$scratch%zu", context->scratch_used);
            context->scratch_used++;
            scratch_slice.start = scratch_name;
            scratch_slice.end = scratch_name + strlen(scratch_name);
            cc_asm_emit_assignment(context, scratch_slice, rhs);

            scratch = cc_asm_lookup_symbol(context, scratch_name);
            if (scratch != NULL && cc_asm_type_is_float(scratch->type)) {
                cc_asm_load_float_into(context, scratch_slice, 0);
                cc_asm_store_place(context, &place, place_type, true);
            } else {
                cc_asm_load_integer_into(context, scratch_slice, 0);
                cc_asm_store_place(context, &place, place_type, false);
            }
            free(scratch_name);
            cc_asm_place_free(&place);
            return;
        }

        if (cc_asm_type_is_float(place_type)) {
            cc_asm_load_float_into(context, rhs, 0);
            cc_asm_store_place(context, &place, place_type, true);
        } else {
            cc_asm_load_integer_into(context, rhs, 0);
            cc_asm_store_place(context, &place, place_type, false);
        }
        cc_asm_place_free(&place);
        return;
    }

    destination = cc_asm_require_local(context, lhs_text);
    free(lhs_text);

    cursor = rhs.start;
    end = rhs.end;
    while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
        cursor++;
    }

    word.start = cursor;
    while (cursor < end && *cursor != ' ' && *cursor != '\t') {
        cursor++;
    }
    word.end = cursor;
    cc_asm_trim(&word);

    rest.start = cursor;
    rest.end = end;
    cc_asm_trim(&rest);

    if (rest.start < rest.end && *rest.start == ',') {
        rest.start++;
        cc_asm_trim(&rest);
    }

    if (cc_asm_slice_equals(word, "load")) {
        CCAsmPlace place;

        if (!cc_asm_parse_place(context, rest, &place)) {
            return;
        }
        cc_asm_set_destination_type(destination, cc_asm_place_load_type(context, &place));
        cc_asm_load_place(context, &place, destination->type);
        if (cc_asm_type_is_float(destination->type)) {
            cc_asm_store_float(context, destination);
        } else {
            cc_asm_store_integer(context, destination);
        }
        cc_asm_place_free(&place);
        return;
    }

    if (cc_asm_slice_equals(word, "addr")) {
        CCAsmPlace place;

        if (!cc_asm_parse_place(context, rest, &place)) {
            return;
        }
        cc_asm_set_destination_type(destination, cc_asm_type_pointer_to(cc_asm_place_object_type(context, &place)));
        cc_asm_place_address(context, &place, false);
        if (!context->failed) {
            cc_asm_emit_line(&context->output, "  movq %%r10, %%rax");
            cc_asm_store_integer(context, destination);
        }
        cc_asm_place_free(&place);
        return;
    }

    if (cc_asm_slice_equals(word, "cast")) {
        operand_count = cc_asm_split_list(rest, operands, 2);
        if (operand_count != 2) {
            cc_asm_error(context, "malformed cast in generated IR");
            return;
        }
        cc_asm_emit_cast(context, destination, operands[0], operands[1]);
        return;
    }

    if (cc_asm_slice_equals(word, "call")) {
        cc_asm_emit_call(context, destination, rest);
        return;
    }

    if (cc_asm_slice_equals(word, "size_of") || cc_asm_slice_equals(word, "kw_sizeof")
        || cc_asm_slice_equals(word, "alignof") || cc_asm_slice_equals(word, "kw__Alignof")) {
        cc_asm_error(context, "x86-64 backend does not support sizeof/alignof expressions yet");
        return;
    }

    if (cc_asm_is_binary_opcode(word) && memchr(rest.start, ',', (size_t)(rest.end - rest.start)) != NULL) {
        operand_count = cc_asm_split_list(rest, operands, 2);
        if (operand_count != 2) {
            cc_asm_error(context, "malformed binary expression in generated IR");
            return;
        }
        cc_asm_emit_binary(context, destination, word, operands[0], operands[1]);
        return;
    }

    if (cc_asm_slice_equals(word, "neg") || cc_asm_slice_equals(word, "bitnot")
        || cc_asm_slice_equals(word, "not") || cc_asm_slice_equals(word, "identity")
        || cc_asm_slice_equals(word, "unary")) {
        cc_asm_emit_unary(context, destination, word, rest);
        return;
    }

    /* Everything else is a copy of a single operand. */
    {
        CCAsmLiteral literal;
        CCAsmSlice whole;

        whole = rhs;
        cc_asm_parse_literal(whole, &literal);
        if (literal.kind == CC_ASM_LITERAL_STRING) {
            const char *label;

            label = cc_asm_string_label(context, literal.bytes, literal.length);
            cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_I8, 1));
            cc_asm_emit_line(&context->output, "  leaq %s(%%rip), %%rax", label);
            cc_asm_store_integer(context, destination);
            cc_asm_literal_free(&literal);
            return;
        }
        if (literal.kind == CC_ASM_LITERAL_FLOAT) {
            cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_F64, 0));
            cc_asm_load_float_into(context, whole, 0);
            cc_asm_store_float(context, destination);
            cc_asm_literal_free(&literal);
            return;
        }
        if (literal.kind == CC_ASM_LITERAL_INTEGER) {
            cc_asm_set_destination_type(destination, cc_asm_type_make(CC_ASM_SCALAR_I64, 0));
            cc_asm_load_integer_into(context, whole, 0);
            cc_asm_store_integer(context, destination);
            cc_asm_literal_free(&literal);
            return;
        }
        cc_asm_literal_free(&literal);

        {
            CCAsmType source_type;

            source_type = cc_asm_operand_type(context, whole);
            cc_asm_set_destination_type(destination, source_type);
            if (cc_asm_type_is_float(source_type) || cc_asm_type_is_float(destination->type)) {
                cc_asm_load_float_into(context, whole, 0);
                cc_asm_store_float(context, destination);
            } else {
                cc_asm_load_integer_into(context, whole, 0);
                cc_asm_store_integer(context, destination);
            }
        }
    }
}

static const char *cc_asm_int_argument_32(int index) {
    static const char *const names[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};

    return names[index];
}

static void cc_asm_int_argument_sized(int index, size_t size, char *buffer, size_t capacity) {
    static const char *const names[6][4] = {
        {"%rdi", "%edi", "%di", "%dil"},
        {"%rsi", "%esi", "%si", "%sil"},
        {"%rdx", "%edx", "%dx", "%dl"},
        {"%rcx", "%ecx", "%cx", "%cl"},
        {"%r8", "%r8d", "%r8w", "%r8b"},
        {"%r9", "%r9d", "%r9w", "%r9b"}
    };
    int column;

    column = size == 8 ? 0 : size == 4 ? 1 : size == 2 ? 2 : 3;
    snprintf(buffer, capacity, "%s", names[index][column]);
}

static void cc_asm_emit_jump(CCAsmContext *context, CCAsmSlice label) {
    char *name;

    name = cc_asm_label_name(context, label);
    cc_asm_emit_line(&context->output, "  jmp %s", name);
    free(name);
}

static void cc_asm_emit_conditional_jump(CCAsmContext *context, CCAsmSlice value, CCAsmSlice then_label, CCAsmSlice else_label) {
    CCAsmType type;
    char *then_name;
    char *else_name;

    then_name = cc_asm_label_name(context, then_label);
    else_name = cc_asm_label_name(context, else_label);
    type = cc_asm_operand_type(context, value);

    if (cc_asm_type_is_float(type)) {
        cc_asm_load_float_into(context, value, 0);
        cc_asm_emit_line(&context->output, "  xorpd %%xmm1, %%xmm1");
        cc_asm_emit_line(&context->output, "  ucomisd %%xmm1, %%xmm0");
        cc_asm_emit_line(&context->output, "  jp %s", then_name);
        cc_asm_emit_line(&context->output, "  jne %s", then_name);
        cc_asm_emit_line(&context->output, "  jmp %s", else_name);
    } else {
        cc_asm_load_integer_into(context, value, 0);
        cc_asm_emit_line(&context->output, "  testq %%rax, %%rax");
        cc_asm_emit_line(&context->output, "  jne %s", then_name);
        cc_asm_emit_line(&context->output, "  jmp %s", else_name);
    }

    free(then_name);
    free(else_name);
}

static void cc_asm_emit_epilogue(CCAsmContext *context) {
    cc_asm_emit_line(&context->output, "  movq %%rbp, %%rsp");
    cc_asm_emit_line(&context->output, "  popq %%rbp");
    cc_asm_emit_line(&context->output, "  ret");
}

static void cc_asm_emit_return(CCAsmContext *context, CCAsmSlice value, bool has_value) {
    if (has_value) {
        if (cc_asm_type_is_float(context->function_return_type)) {
            cc_asm_load_float_into(context, value, 0);
        } else {
            cc_asm_load_integer_into(context, value, 0);
        }
    }

    cc_asm_emit_epilogue(context);
}

static bool cc_asm_is_place_text(const char *text) {
    const char *dot;
    const char *suffix;

    if (text == NULL) {
        return false;
    }
    if (text[0] == '*' || strchr(text, '[') != NULL) {
        return true;
    }

    dot = strchr(text, '.');
    if (dot == NULL) {
        return false;
    }

    /* A trailing all-digit suffix is the IR's rename for a shadowed local
     * (x.0), not a struct member (s.a). */
    suffix = strrchr(text, '.') + 1;
    if (*suffix == '\0') {
        return true;
    }
    for (; *suffix != '\0'; suffix++) {
        if (!isdigit((unsigned char)*suffix)) {
            return true;
        }
    }

    return false;
}

static bool cc_asm_declarator_is_array(const CCAstNode *declarator) {
    size_t index;

    if (declarator == NULL) {
        return false;
    }

    for (index = 0; index < declarator->child_count; index++) {
        if (declarator->children[index]->kind == CC_AST_ARRAY_DECLARATOR) {
            return true;
        }
        if (declarator->children[index]->kind == CC_AST_DECLARATOR
            && cc_asm_declarator_is_array(declarator->children[index])) {
            return true;
        }
    }

    return false;
}

static bool cc_asm_declares_array(const CCAstNode *node, const char *name) {
    size_t index;

    if (node == NULL || name == NULL) {
        return false;
    }

    if (node->kind == CC_AST_DECLARATION) {
        size_t child_index;

        for (child_index = 1; child_index < node->child_count; child_index++) {
            const CCAstNode *init_declarator;

            init_declarator = node->children[child_index];
            if (init_declarator->kind != CC_AST_INIT_DECLARATOR || init_declarator->child_count == 0) {
                continue;
            }
            if (init_declarator->children[0]->text != NULL
                && strcmp(init_declarator->children[0]->text, name) == 0
                && cc_asm_declarator_is_array(init_declarator->children[0])) {
                return true;
            }
        }
    }

    for (index = 0; index < node->child_count; index++) {
        if (cc_asm_declares_array(node->children[index], name)) {
            return true;
        }
    }

    return false;
}

static void cc_asm_prescan_body(CCAsmContext *context, char **lines, size_t line_count) {
    size_t index;

    for (index = 0; index < line_count; index++) {
        const char *cursor;
        CCAsmSlice line;
        CCAsmSlice word;

        line.start = lines[index];
        line.end = lines[index] + strlen(lines[index]);
        cc_asm_trim(&line);
        if (line.start >= line.end) {
            continue;
        }

        if (line.end[-1] == ':') {
            continue;
        }

        cursor = line.start;
        word.start = cursor;
        while (cursor < line.end && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
        word.end = cursor;

        if (cc_asm_slice_equals(word, "var")) {
            CCAsmSlice name;
            CCAsmSlice type_text;
            char *name_text;
            char *type_string;

            while (cursor < line.end && (*cursor == ' ' || *cursor == '\t')) {
                cursor++;
            }
            name.start = cursor;
            while (cursor < line.end && *cursor != ' ' && *cursor != ':' && *cursor != '\t') {
                cursor++;
            }
            name.end = cursor;
            while (cursor < line.end && (*cursor == ' ' || *cursor == '\t')) {
                cursor++;
            }
            if (cursor >= line.end || *cursor != ':') {
                continue;
            }
            cursor++;
            while (cursor < line.end && (*cursor == ' ' || *cursor == '\t')) {
                cursor++;
            }
            type_text.start = cursor;
            type_text.end = line.end;

            name_text = cc_asm_slice_dup(name);
            type_string = cc_asm_slice_dup(type_text);
            if (cc_asm_declares_array(context->function_node, name_text)) {
                cc_asm_error(context, "x86-64 backend does not support local arrays yet ('%s')", name_text);
                free(name_text);
                free(type_string);
                return;
            }
            if (cc_asm_lookup_symbol(context, name_text) == NULL) {
                CCAsmSymbol *symbol;

                symbol = cc_asm_add_local(context, name_text, cc_asm_type_from_text(context, type_string));
                symbol->declared = true;
            }
            free(name_text);
            free(type_string);
            continue;
        }

        {
            const char *equals;
            CCAsmSlice lhs;
            CCAsmSlice rhs;
            char *lhs_text;

            lhs.start = line.start;
            lhs.end = line.end;
            equals = NULL;
            for (; cursor < line.end; cursor++) {
                if (*cursor == '=') {
                    equals = cursor;
                    break;
                }
            }
            if (equals == NULL) {
                continue;
            }

            lhs.end = equals;
            cc_asm_trim(&lhs);
            rhs.start = equals + 1;
            rhs.end = line.end;
            cc_asm_trim(&rhs);

            lhs_text = cc_asm_slice_dup(lhs);
            if (cc_asm_is_place_text(lhs_text)) {
                if (rhs.start < rhs.end && !cc_asm_is_simple_value(rhs)) {
                    context->scratch_count++;
                }
            } else if (cc_asm_lookup_symbol(context, lhs_text) == NULL) {
                cc_asm_add_local(context, lhs_text, cc_asm_type_make(CC_ASM_SCALAR_I64, 0));
            }
            free(lhs_text);
        }
    }
}

static size_t cc_asm_parameter_width(CCAsmType type) {
    size_t size;

    size = cc_asm_type_size(type);
    return size == 0 ? 8 : size;
}

static void cc_asm_emit_parameter(CCAsmContext *context, const CCAsmSymbol *symbol, int integer_index, int float_index) {
    char *memory;
    char registry[16];
    size_t size;

    memory = cc_asm_symbol_memory(symbol);
    size = cc_asm_parameter_width(symbol->type);

    if (cc_asm_type_is_float(symbol->type)) {
        cc_asm_emit_line(&context->output, "  movsd %%xmm%d, %s", float_index, memory);
    } else {
        cc_asm_int_argument_sized(integer_index, size, registry, sizeof(registry));
        if (size == 8) {
            cc_asm_emit_line(&context->output, "  movq %s, %s", registry, memory);
        } else if (size == 4) {
            cc_asm_emit_line(&context->output, "  movl %s, %s", registry, memory);
        } else if (size == 2) {
            cc_asm_emit_line(&context->output, "  movw %s, %s", registry, memory);
        } else {
            cc_asm_emit_line(&context->output, "  movb %s, %s", registry, memory);
        }
    }

    free(memory);
}

static void cc_asm_emit_function(CCAsmContext *context, CCAsmSlice signature, char **lines, size_t line_count) {
    CCAsmSlice name_slice;
    CCAsmSlice parameter_slice;
    CCAsmSlice return_slice;
    const char *open;
    const char *arrow;
    char *name;
    CCAsmType return_type;
    CCAsmSlice parameters[32];
    size_t parameter_count;
    size_t index;
    int frame_size;
    int integer_index;
    int float_index;
    bool had_terminator;

    open = signature.start;
    while (open < signature.end && *open != '(') {
        open++;
    }

    name_slice.start = signature.start;
    name_slice.end = open;
    cc_asm_trim(&name_slice);
    name = cc_asm_slice_dup(name_slice);

    arrow = signature.start;
    while (arrow + 1 < signature.end && !(arrow[0] == '-' && arrow[1] == '>')) {
        arrow++;
    }
    if (arrow + 1 >= signature.end) {
        cc_asm_error(context, "malformed function signature in generated IR");
        free(name);
        return;
    }

    return_slice.start = arrow + 2;
    return_slice.end = signature.end;
    cc_asm_trim(&return_slice);
    {
        char *return_text;

        return_text = cc_asm_slice_dup(return_slice);
        return_type = cc_asm_type_from_text(context, return_text);
        free(return_text);
    }

    parameter_count = 0;
    if (open < signature.end) {
        const char *close;

        close = signature.end;
        while (close > open) {
            if (close[-1] == ')') {
                break;
            }
            close--;
        }
        if (close > open + 1) {
            parameter_slice.start = open + 1;
            parameter_slice.end = close - 1;
            parameter_count = cc_asm_split_list(parameter_slice, parameters, sizeof(parameters) / sizeof(parameters[0]));
            if (parameter_count == 1 && cc_asm_slice_equals(parameters[0], "void")) {
                parameter_count = 0;
            }
        }
    }

    context->function_name = name;
    context->function_node = cc_asm_find_function_node(context->parse_result, name);
    context->function_return_type = return_type;
    cc_asm_reset_locals(context);
    context->slot_count = 0;
    context->failed = false;

    for (index = 0; index < parameter_count; index++) {
        const char *colon;
        CCAsmSlice parameter_name;
        CCAsmSlice parameter_type_slice;
        char *parameter_type_text;
        char *parameter_name_text;
        CCAsmType parameter_type;
        CCAsmSymbol *symbol;

        if (cc_asm_slice_equals(parameters[index], "...")) {
            continue;
        }

        colon = NULL;
        {
            const char *cursor;

            for (cursor = parameters[index].start; cursor < parameters[index].end; cursor++) {
                if (*cursor == ':') {
                    colon = cursor;
                }
            }
        }
        if (colon == NULL) {
            continue;
        }

        parameter_name.start = parameters[index].start;
        parameter_name.end = colon;
        cc_asm_trim(&parameter_name);
        parameter_type_slice.start = colon + 1;
        parameter_type_slice.end = parameters[index].end;
        cc_asm_trim(&parameter_type_slice);

        parameter_name_text = cc_asm_slice_dup(parameter_name);
        parameter_type_text = cc_asm_slice_dup(parameter_type_slice);
        parameter_type = cc_asm_type_from_text(context, parameter_type_text);
        free(parameter_type_text);

        if (cc_asm_type_is_aggregate(parameter_type)) {
            cc_asm_error(context, "x86-64 backend does not support struct parameters by value yet");
            free(parameter_name_text);
            free(name);
            return;
        }

        symbol = cc_asm_add_local(context, parameter_name_text, parameter_type);
        symbol->declared = true;
        free(parameter_name_text);
    }

    cc_asm_prescan_body(context, lines, line_count);
    if (context->failed) {
        free(name);
        return;
    }

    for (index = 0; index < context->scratch_count; index++) {
        char *scratch_name;

        scratch_name = cc_format_string("$scratch%zu", index);
        cc_asm_add_local(context, scratch_name, cc_asm_type_make(CC_ASM_SCALAR_I64, 0));
        free(scratch_name);
    }
    context->scratch_used = 0;

    frame_size = context->slot_count * 8;
    if (frame_size % 16 != 0) {
        frame_size = (frame_size + 15) / 16 * 16;
    }
    if (frame_size > 2048) {
        cc_asm_error(context, "x86-64 backend frame for '%s' exceeds the %d byte slot limit", name, 2048);
        free(name);
        return;
    }

    /* Calls appear in other functions, so a definition has to publish its
     * own signature the same way a declaration does. */
    cc_asm_register_function(context, name, return_type, false);

    cc_asm_emit_line(&context->output, "");
    cc_asm_emit_line(&context->output, ".p2align 4");
    cc_asm_emit_line(&context->output, ".globl %s%s", CC_ASM_GLOBAL_PREFIX, name);
    cc_asm_emit_line(&context->output, "%s%s:", CC_ASM_GLOBAL_PREFIX, name);
    cc_asm_emit_line(&context->output, "  pushq %%rbp");
    cc_asm_emit_line(&context->output, "  movq %%rsp, %%rbp");
    if (frame_size > 0) {
        cc_asm_emit_line(&context->output, "  subq $%d, %%rsp", frame_size);
    }

    integer_index = 0;
    float_index = 0;
    for (index = 0; index < context->local_count; index++) {
        if (index < parameter_count) {
            const CCAsmSymbol *symbol;

            symbol = &context->locals[index];
            if (cc_asm_type_is_float(symbol->type)) {
                cc_asm_emit_parameter(context, symbol, integer_index, float_index);
                float_index++;
            } else {
                cc_asm_emit_parameter(context, symbol, integer_index, float_index);
                integer_index++;
            }
        }
    }

    had_terminator = false;
    for (index = 0; index < line_count; index++) {
        CCAsmSlice line;
        CCAsmSlice word;
        const char *cursor;

        if (context->failed) {
            free(name);
            return;
        }

        line.start = lines[index];
        line.end = lines[index] + strlen(lines[index]);
        cc_asm_trim(&line);
        if (line.start >= line.end) {
            continue;
        }

        if (line.end[-1] == ':') {
            CCAsmSlice label;
            char *label_name_text;

            label.start = line.start;
            label.end = line.end - 1;
            cc_asm_trim(&label);
            label_name_text = cc_asm_label_name(context, label);
            cc_asm_emit_line(&context->output, "%s:", label_name_text);
            free(label_name_text);
            had_terminator = false;
            continue;
        }

        cursor = line.start;
        word.start = cursor;
        while (cursor < line.end && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
        word.end = cursor;

        if (cc_asm_slice_equals(word, "jump")) {
            CCAsmSlice target;

            target.start = cursor;
            target.end = line.end;
            cc_asm_trim(&target);
            cc_asm_emit_jump(context, target);
            had_terminator = true;
            continue;
        }

        if (cc_asm_slice_equals(word, "cjump")) {
            CCAsmSlice operands[3];
            CCAsmSlice argument_slice;
            size_t count;

            argument_slice.start = cursor;
            argument_slice.end = line.end;
            count = cc_asm_split_list(argument_slice, operands, 3);
            if (count != 3) {
                cc_asm_error(context, "malformed cjump in generated IR");
                free(name);
                return;
            }
            cc_asm_emit_conditional_jump(context, operands[0], operands[1], operands[2]);
            had_terminator = true;
            continue;
        }

        if (cc_asm_slice_equals(word, "ret")) {
            CCAsmSlice value;
            bool has_value;

            value.start = cursor;
            value.end = line.end;
            cc_asm_trim(&value);
            has_value = value.start < value.end;
            cc_asm_emit_return(context, value, has_value);
            had_terminator = true;
            continue;
        }

        if (cc_asm_slice_equals(word, "var")) {
            continue;
        }

        {
            CCAsmSlice lhs;
            CCAsmSlice rhs;
            const char *equals;

            lhs.start = line.start;
            lhs.end = line.end;
            equals = NULL;
            for (const char *scan = cursor; scan < line.end; scan++) {
                if (*scan == '=') {
                    equals = scan;
                    break;
                }
            }
            if (equals == NULL) {
                cc_asm_error(context, "unrecognized IR statement in generated IR");
                free(name);
                return;
            }
            lhs.end = equals;
            cc_asm_trim(&lhs);
            rhs.start = equals + 1;
            rhs.end = line.end;
            cc_asm_trim(&rhs);
            cc_asm_emit_assignment(context, lhs, rhs);
            had_terminator = false;
        }
    }

    if (context->failed) {
        free(name);
        return;
    }

    if (!had_terminator) {
        if (!cc_asm_type_is_float(return_type) && return_type.scalar != CC_ASM_SCALAR_VOID) {
            cc_asm_emit_line(&context->output, "  xorl %%eax, %%eax");
        }
        cc_asm_emit_epilogue(context);
    }

    context->function_name = NULL;
    context->function_node = NULL;
    free(name);
}

static const CCAstNode *cc_asm_find_function_node(const CCParseResult *parse_result, const char *name) {
    size_t index;

    if (parse_result == NULL || parse_result->translation_unit == NULL || name == NULL) {
        return NULL;
    }

    for (index = 0; index < parse_result->translation_unit->child_count; index++) {
        const CCAstNode *child;

        child = parse_result->translation_unit->children[index];
        if (child->kind == CC_AST_FUNCTION_DEFINITION && child->text != NULL && strcmp(child->text, name) == 0) {
            return child;
        }
    }

    return NULL;
}

static char **cc_asm_split_lines(const char *text, size_t *count_out) {
    char **lines;
    size_t count;
    size_t index;
    const char *cursor;

    count = 1;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\n') {
            count++;
        }
    }

    lines = cc_reallocate_or_die(NULL, count * sizeof(*lines));
    index = 0;
    cursor = text;
    while (index < count) {
        const char *line_start;
        size_t length;

        line_start = cursor;
        while (*cursor != '\0' && *cursor != '\n') {
            cursor++;
        }
        length = (size_t)(cursor - line_start);
        lines[index] = cc_duplicate_range(line_start, length);
        index++;
        if (*cursor == '\n') {
            cursor++;
        }
    }

    *count_out = count;
    return lines;
}

static void cc_asm_emit_globals(CCAsmContext *context) {
    size_t index;

    if (context->global_count == 0) {
        return;
    }

    cc_asm_emit_line(&context->output, "");
#ifdef __APPLE__
    cc_asm_emit_line(&context->output, ".section __DATA,__data");
#else
    cc_asm_emit_line(&context->output, ".section .data");
#endif

    for (index = 0; index < context->global_count; index++) {
        size_t size;

        size = cc_asm_type_size(context->globals[index].type);
        if (size == 0) {
            size = 8;
        }

        cc_asm_emit_line(&context->output, ".p2align 3");
        cc_asm_emit_line(&context->output, "%s%s:", CC_ASM_GLOBAL_PREFIX, context->globals[index].name);
        if (context->globals[index].has_initializer) {
            if (size == 1) {
                cc_asm_emit_line(&context->output, "  .byte %lld", context->globals[index].initializer);
            } else if (size == 2) {
                cc_asm_emit_line(&context->output, "  .value %lld", context->globals[index].initializer);
            } else if (size == 4) {
                cc_asm_emit_line(&context->output, "  .long %lld", context->globals[index].initializer);
            } else {
                cc_asm_emit_line(&context->output, "  .quad %lld", context->globals[index].initializer);
            }
        } else {
            cc_asm_emit_line(&context->output, "  .space %zu", size);
        }
    }
}

static void cc_asm_emit_constants(CCAsmContext *context) {
    size_t index;

    if (context->string_count > 0) {
        cc_asm_emit_line(&context->output, "");
        cc_asm_emit_line(&context->output, "%s", CC_ASM_RODATA_HEADER);

        for (index = 0; index < context->string_count; index++) {
            CCAsmBuilder bytes;
            const CCAsmString *entry;
            size_t offset;

            entry = &context->strings[index];
            cc_asm_emit_line(&context->output, "%s:", entry->label);

            memset(&bytes, 0, sizeof(bytes));
            for (offset = 0; offset <= entry->length; offset += 16) {
                size_t limit;
                size_t position;
                char item[8];

                limit = entry->length + 1 - offset;
                if (limit > 16) {
                    limit = 16;
                }

                cc_asm_builder_append_cstring(&bytes, "  .byte ");
                for (position = 0; position < limit; position++) {
                    snprintf(item, sizeof(item), "%s%u", position == 0 ? "" : ",", (unsigned)(unsigned char)entry->bytes[offset + position]);
                    cc_asm_builder_append_cstring(&bytes, item);
                }
                cc_asm_builder_append_cstring(&bytes, "\n");
            }
            cc_asm_builder_append_cstring(&context->output, bytes.data == NULL ? "" : bytes.data);
            free(bytes.data);
        }
    }

    if (context->double_count > 0) {
        cc_asm_emit_line(&context->output, "");
#ifdef __APPLE__
        cc_asm_emit_line(&context->output, ".section __TEXT,__const");
#else
        cc_asm_emit_line(&context->output, ".section .rodata");
#endif

        for (index = 0; index < context->double_count; index++) {
            cc_asm_emit_line(&context->output, ".p2align 3");
            cc_asm_emit_line(&context->output, "%s:", context->doubles[index].label);
            cc_asm_emit_line(&context->output, "  .quad 0x%016llx", (unsigned long long)context->doubles[index].bits);
        }
    }
}

static void cc_asm_register_declaration(CCAsmContext *context, CCAsmSlice declaration) {
    CCAsmSlice text;
    CCAsmSlice name_slice;
    CCAsmSlice return_slice;
    const char *open;
    const char *arrow;
    char *name;
    char *return_text;
    bool variadic;

    text = declaration;
    cc_asm_trim(&text);

    open = text.start;
    while (open < text.end && *open != '(') {
        open++;
    }
    if (open >= text.end) {
        return;
    }

    name_slice.start = text.start;
    name_slice.end = open;
    cc_asm_trim(&name_slice);

    arrow = open;
    while (arrow + 1 < text.end && !(arrow[0] == '-' && arrow[1] == '>')) {
        arrow++;
    }
    if (arrow + 1 >= text.end) {
        return;
    }

    return_slice.start = arrow + 2;
    return_slice.end = text.end;
    cc_asm_trim(&return_slice);

    variadic = false;
    {
        const char *cursor;

        for (cursor = open; cursor + 2 < arrow; cursor++) {
            if (cursor[0] == '.' && cursor[1] == '.' && cursor[2] == '.') {
                variadic = true;
                break;
            }
        }
    }

    name = cc_asm_slice_dup(name_slice);
    return_text = cc_asm_slice_dup(return_slice);
    cc_asm_register_function(context, name, cc_asm_type_from_text(context, return_text), variadic);
    free(return_text);
    free(name);
}

void cc_x86_translation_unit(const CCParseResult *parse_result, const char *ir_text, CCX86Result *result) {
    CCAsmContext context;
    char **lines;
    size_t line_count;
    size_t index;

    memset(&context, 0, sizeof(context));
    memset(result, 0, sizeof(*result));
    context.parse_result = parse_result;

    cc_asm_collect_typedefs(&context);
    lines = cc_asm_split_lines(ir_text == NULL ? "" : ir_text, &line_count);

    cc_asm_emit_line(&context.output, "# generated by C-Compiler --target=x86_64");
    cc_asm_emit_line(&context.output, ".text");

    index = 0;
    while (index < line_count) {
        CCAsmSlice line;
        CCAsmSlice word;
        const char *cursor;

        line.start = lines[index];
        line.end = lines[index] + strlen(lines[index]);
        cc_asm_trim(&line);
        index++;
        if (line.start >= line.end) {
            continue;
        }

        cursor = line.start;
        word.start = cursor;
        while (cursor < line.end && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
        word.end = cursor;

        if (cc_asm_slice_equals(word, "declare")) {
            CCAsmSlice declaration;

            declaration.start = cursor;
            declaration.end = line.end;
            cc_asm_register_declaration(&context, declaration);
            continue;
        }

        if (cc_asm_slice_equals(word, "global")) {
            CCAsmSlice name_slice;
            CCAsmSlice type_slice;
            char *name_text;
            char *type_text;
            CCAsmSymbol *symbol;

            while (cursor < line.end && (*cursor == ' ' || *cursor == '\t')) {
                cursor++;
            }
            name_slice.start = cursor;
            while (cursor < line.end && *cursor != ' ' && *cursor != ':' && *cursor != '\t') {
                cursor++;
            }
            name_slice.end = cursor;
            while (cursor < line.end && (*cursor == ' ' || *cursor == ':' || *cursor == '\t')) {
                cursor++;
            }
            type_slice.start = cursor;
            type_slice.end = line.end;

            name_text = cc_asm_slice_dup(name_slice);
            type_text = cc_asm_slice_dup(type_slice);
            symbol = cc_asm_add_symbol(&context.globals, &context.global_count, &context.global_capacity, name_text);
            symbol->type = cc_asm_type_from_text(&context, type_text);
            symbol->global_label = cc_format_string("%s%s", CC_ASM_GLOBAL_PREFIX, name_text);
            free(name_text);
            free(type_text);
            continue;
        }

        {
            const char *equals;
            CCAsmSlice lhs;
            char *lhs_text;

            equals = NULL;
            for (const char *scan = line.start; scan < line.end; scan++) {
                if (*scan == '=') {
                    equals = scan;
                    break;
                }
            }
            if (equals != NULL) {
                CCAsmSlice rhs;
                CCAsmLiteral literal;
                const CCAsmSymbol *target;

                lhs.start = line.start;
                lhs.end = equals;
                cc_asm_trim(&lhs);
                lhs_text = cc_asm_slice_dup(lhs);
                target = cc_asm_lookup_symbol(&context, lhs_text);
                if (target != NULL && target->slot < 0) {
                    size_t position;

                    for (position = 0; position < context.global_count; position++) {
                        if (strcmp(context.globals[position].name, lhs_text) == 0) {
                            break;
                        }
                    }

                    rhs.start = equals + 1;
                    rhs.end = line.end;
                    cc_asm_trim(&rhs);
                    cc_asm_parse_literal(rhs, &literal);
                    if (literal.kind != CC_ASM_LITERAL_INTEGER) {
                        cc_asm_error(&context, "x86-64 backend only supports integer global initializers yet");
                        cc_asm_literal_free(&literal);
                        free(lhs_text);
                        break;
                    }
                    context.globals[position].has_initializer = true;
                    context.globals[position].initializer = literal.integer;
                    cc_asm_literal_free(&literal);
                    free(lhs_text);
                    continue;
                }
                free(lhs_text);
            }
        }

        if (cc_asm_slice_equals(word, "func")) {
            CCAsmSlice signature;
            size_t body_start;
            size_t body_end;

            signature.start = cursor;
            signature.end = line.end;
            body_start = index;
            body_end = index;
            while (body_end < line_count) {
                CCAsmSlice candidate;
                CCAsmSlice candidate_word;
                const char *scan;

                candidate.start = lines[body_end];
                candidate.end = lines[body_end] + strlen(lines[body_end]);
                cc_asm_trim(&candidate);
                scan = candidate.start;
                candidate_word.start = scan;
                while (scan < candidate.end && *scan != ' ' && *scan != '\t') {
                    scan++;
                }
                candidate_word.end = scan;
                if (cc_asm_slice_equals(candidate_word, "endfunc")) {
                    break;
                }
                body_end++;
            }

            cc_asm_emit_function(&context, signature, lines + body_start, body_end - body_start);
            index = body_end < line_count ? body_end + 1 : line_count;
            continue;
        }
    }

    cc_asm_emit_globals(&context);
    cc_asm_emit_constants(&context);

    result->text = cc_asm_builder_take(&context.output);
    result->diagnostics = context.diagnostics;

    for (index = 0; index < context.local_count; index++) {
        free(context.locals[index].name);
        free(context.locals[index].global_label);
    }
    free(context.locals);
    for (index = 0; index < context.global_count; index++) {
        free(context.globals[index].name);
        free(context.globals[index].global_label);
    }
    free(context.globals);
    for (index = 0; index < context.typedef_count; index++) {
        free(context.typedefs[index].name);
        free(context.typedefs[index].spelled);
    }
    free(context.typedefs);
    for (index = 0; index < context.function_count; index++) {
        free(context.functions[index].name);
    }
    free(context.functions);
    for (index = 0; index < context.string_count; index++) {
        free(context.strings[index].label);
        free(context.strings[index].bytes);
    }
    free(context.strings);
    for (index = 0; index < context.double_count; index++) {
        free(context.doubles[index].label);
    }
    free(context.doubles);
    for (index = 0; index < line_count; index++) {
        free(lines[index]);
    }
    free(lines);
}

void cc_x86_result_free(CCX86Result *result) {
    size_t index;

    for (index = 0; index < result->diagnostics.count; index++) {
        free(result->diagnostics.items[index].path);
        free(result->diagnostics.items[index].message);
    }

    free(result->diagnostics.items);
    result->diagnostics.items = NULL;
    result->diagnostics.count = 0;
    result->diagnostics.capacity = 0;

    free(result->text);
    result->text = NULL;
}
