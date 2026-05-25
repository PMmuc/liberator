int testfunc(int *n) {
  if (*n == 1)
    return 1;
  return (*n) * testfunc(n);
}

int main(int argc, char **argv) { return testfunc(&argc); }
