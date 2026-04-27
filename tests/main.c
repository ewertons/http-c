#include <stdlib.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "task.h"

#include <test_http.h>

int main()
{
  int result = 0;

  result += test_http_headers();
  result += test_http_request();
  result += test_http_request_parser();
  result += test_http_response();
  task_platform_init();
  result += test_http_endpoint();
  result += test_http_server();
  task_platform_deinit();

  return result;
}