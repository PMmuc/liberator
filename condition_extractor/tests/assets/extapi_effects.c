// External API effect models must reach the bottom-up summary.
//
// memcpy/strlen have no body the bottom-up analysis could summarize; their
// effect is only described by the hand-written models in accessTypeHandlers.
// merge_summary has to dispatch to them at the call boundary, otherwise both
// parameters below come back as plain reads without the array flag, and the
// length dependency (dst/src length is param 2) is lost.

#include <string.h>

// dst and src are memcpy'd with an explicit length parameter -> both are
// arrays, and their length depends on `len`.
void copy_buffer(char *dst, const char *src, unsigned long len) {
  memcpy(dst, src, len);
}

// s is consumed by strlen -> array, no explicit length parameter.
unsigned long measure(const char *s) { return strlen(s); }

int main(void) {
  char a[16];
  const char *b = "hello";
  copy_buffer(a, b, 5);
  return (int)measure(a);
}
