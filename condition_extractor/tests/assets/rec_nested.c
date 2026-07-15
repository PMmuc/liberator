int inner_rec(int *y) {
  if (*y <= 0)
    return 1;
  (*y)--;
  return 2 * inner_rec(y);
}

int middle_func(int *x) {
  return inner_rec(x);
}

int outer_rec(int *z) {
  if (*z <= 0)
    return 0;
  return middle_func(z) + outer_rec(z);
}
