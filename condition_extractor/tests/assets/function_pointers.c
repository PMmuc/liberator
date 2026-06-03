#include <dlfcn.h>
#include <stddef.h>

int f1(int a, void *b) { return 0; }
int f2(int a, void *b) { return 0; }
int f3(int a, void *b) { return 0; }

typedef int (*cb_t)(int, void *);

cb_t global1;
cb_t global2;
cb_t global3 = &f3;
cb_t global5;
cb_t global8;

typedef void *(*malloc_t)(size_t);
malloc_t global_malloc;

extern int global6;
extern int a;

typedef struct {
  cb_t func;
} test_t;

// Constant aggregate
test_t global4 = {&f2};

extern cb_t populate_func_ptr();

void test_func(cb_t c) {
  global1 = populate_func_ptr();

  int a = 3;
  // unresolved function call pointer.
  // because populate_func_ptr has external linkage
  global1(10, &a);
  if (a == 4)
    global2 = &f1;
  else
    global2 = &f2;
  global2(20, &a);

  // unresolved function call pointer.
  // because no definition at all.
  global8(10, &a);

  void *handle = dlopen("libc.so.6", RTLD_LAZY);
  if (handle) {
    global_malloc = (malloc_t)dlsym(handle, "malloc");
  }

  // unresolved function call pointer.
  // because global_malloc is also defined by external function.
  void *alloc = global_malloc(20);
}
