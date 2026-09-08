// Exercises the ScalarEvolution-based length detection
// (extractLenDependencyParameterSCEV). Compiled at -O1 without debug info
// (nodwarf_ prefix) so parameters stay in SSA registers and loops keep
// analyzable induction variables — at -O0 every value round-trips through
// an alloca and SCEV sees nothing.

// classic upward loop: n strictly bounds every index into buf
void fill_upcount(int *buf, int n) {
  for (int i = 0; i < n; i++)
    buf[i] = i;
}

// no loop: the bound comes from the dominating branch condition
void guarded_store(int *buf, int n, int i) {
  if (i < n)
    buf[i] = 7;
}

// pointer-bump walk: no index value exists, only the pointer offset.
// The stored value varies so -O1 cannot turn the loop into a memset.
void byte_walk(unsigned char *dst, int n) {
  while (n-- > 0) {
    *dst = (unsigned char)n;
    dst++;
  }
}

// n flows into buf but never bounds an index
void no_len(int *buf, int n) { buf[0] = n; }
