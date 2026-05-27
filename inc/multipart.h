#ifndef GOGOSHARE_MULTIPART_H
#define GOGOSHARE_MULTIPART_H

#include <stdbool.h>

#include <span.h>
#include <niceties.h>

typedef struct mp_part
{
    span_t name;
    span_t filename;
    span_t content_type;
    span_t data;
} mp_part_t;

typedef struct mp_iter
{
    span_t boundary;
    span_t cursor;
    bool   done;
} mp_iterator_t;

result_t mp_iterator_init(mp_iterator_t* it, span_t content_type_header, span_t body);
result_t mp_iterator_next(mp_iterator_t* it, mp_part_t* out);

#endif
