void copy_rec(int *src, int *dst, int count) {
  if (count <= 0)
    return;
  *dst = *src;
  copy_rec(src + 1, dst + 1, count - 1);
}
