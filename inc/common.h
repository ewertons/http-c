#ifndef COMMON_H
#define COMMON_H

typedef enum HL_RESULT_ENUM
{
    HL_RESULT_OK,
    HL_RESULT_INVALID_ARG,
    HL_RESULT_EOF,
    HL_RESULT_NOT_FOUND,
    HL_RESULT_ERROR,
    HL_RESULT_BUFFER_OVERFLOW
} HL_RESULT;

static const span_t crlf = span_from_str_literal("\r\n");
static const span_t name_value_separator = span_from_str_literal(": ");
static const span_t space = span_from_str_literal(" ");

#endif // COMMON_H