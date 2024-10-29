#ifndef SSL_H
#define SSL_H

#include "span.h"

typedef struct ssl ssl_t;

void ssl_init();
ssl_t* ssl_new();
void ssl_accept();
int ssl_read(ssl_t* ssl1, span_t buffer, span_t* out_read);
int ssl_write(ssl_t* ssl1, span_t data);



#endif // SSL_H
