class A {
public:
  A *next;
  int b;
};

A global1;

int testfunc(A *a, A *b, int a1) {
  a->next = b;
  a->b = 8;
  return 0;
}

int main(int argc, char **argv) {
  A *global;
  if (argc < 1)
    global = new A;
  else
    global = &global1;

  global->next = nullptr;
  global->b = 3;

  testfunc(nullptr, &global1, 14);
  return 0;
}
