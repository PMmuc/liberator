struct A {
  int f1;
  int f2;
  int test;
};

struct B {
  int f1;
  struct A *a;
};

extern void *mymalloc(int size);

int foo(struct A *a) { return a->f1; }

int test_func(struct B *ptr) {
  int ret = ptr->a->test;

  int a = 1;
  int b = 2;

  int *q = (int *)mymalloc(8);
  int *p = &ptr->f1;

  foo(ptr->a);

  *q = *p;

  return ret;
}
