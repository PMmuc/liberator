extern void *malloc(unsigned int size);

struct Buffer {
  int len;
  char *data;
};

/*
 * The allocation is spilled into a named local before being returned, which is
 * the shape clang emits at -O0:
 *
 *   %b = alloca ptr
 *   %call = call ptr @malloc(i64 16)
 *   store ptr %call, ptr %b          ; spill into the local
 *   ...
 *   %0 = load ptr, ptr %b
 *   ret ptr %0
 *
 * Under typed pointers (LLVM <= 14) clang put a `bitcast i8* %call to
 * %struct.Buffer*` between the call and the store, and the return analysis
 * recognised the allocation by matching that cast's destination type against
 * the function's return type. With opaque pointers there is no cast at all and
 * every pointer is `ptr`, so the malloc can only be tied to the return by
 * following the value flow malloc -> store -> load -> ret, which crosses the
 * alloca and therefore needs the SVFG's memory-SSA edges.
 */
struct Buffer *make_buffer(int len) {
  struct Buffer *b = (struct Buffer *)malloc(sizeof(struct Buffer));
  b->len = len;
  b->data = 0;
  return b;
}

/*
 * Control case: the same allocation returned directly, with no local in
 * between. This is a plain def-use chain from the call to the `ret` and needs
 * no memory-SSA edges, so it isolates whether a failure of make_buffer above
 * is about the store/load hop specifically.
 */
struct Buffer *make_buffer_direct(void) {
  return (struct Buffer *)malloc(sizeof(struct Buffer));
}
