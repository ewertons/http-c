#include <stdlib.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <test_http.h>

int main()
{
  int result = 0;

  result += test_http_headers();
  result += test_http_request();
  result += test_http_response();

  return result;
}