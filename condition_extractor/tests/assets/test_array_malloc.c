#include <stdlib.h>
#include <string.h>

char *init_buffer(int size, const char *src) {
  char *buf = (char *)malloc(size);
  if (buf && src) {
    memcpy(buf, src, size);
    memset(buf, 0, size);
  }
  return buf;
}

char *test_fun(int n, char *input_arr) {
  if (n <= 0)
    return NULL;
  return init_buffer(n, input_arr);
}
