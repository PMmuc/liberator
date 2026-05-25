#include <stdlib.h>
#include <string.h>

int fun3(char *w) {
  int i = strlen(w);
  return i;
}
char *fun2(char *z) {
  int l = fun3(z);
  return (char *)malloc(l);
}
char *fun1(char *y) { return fun2(y); }

char *test_fun(char *x) { return fun1(x); }
