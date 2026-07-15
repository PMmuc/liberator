int sum(int *n) {
  if (*n <= 0)
    return 0;
  (*n)--;
  return *n + sum(n);
}
