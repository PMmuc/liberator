int bar(int *b);

int foo(int *a) {
  if (*a <= 0)
    return 0;
  (*a)--;
  return bar(a);
}

int bar(int *b) {
  if (*b <= 0)
    return 1;
  return foo(b) + 1;
}
