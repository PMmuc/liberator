#include <stdlib.h>
#include <string.h>

int *fun3(int w) { return (int *)malloc(w); }
int *fun2(int z) { return fun3(z); }
int *fun1(int y) { return fun2(y); }

int *test_fun(int x) { return fun1(x); }
