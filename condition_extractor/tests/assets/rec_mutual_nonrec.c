void add_helper(int *a, int val) {
  *a += val;
}

int check_helper(int *b) {
  return *b > 0;
}

void reset_helper(int *c) {
  *c = 0;
}

int rec_bar(int *x, int *y);

int rec_foo(int *x, int *y) {
  if (!check_helper(x)) {
    reset_helper(y);
    return 0;
  }
  add_helper(x, -1);
  add_helper(y, 2);
  return rec_bar(y, x) + 1;
}

int rec_bar(int *x, int *y) {
  if (!check_helper(y)) {
    return 1;
  }
  add_helper(y, -1);
  add_helper(x, 3);
  return rec_foo(y, x) + 2;
}
