#include <stdlib.h>

struct S {
  int a;
  int b;
};

// Control: one level of indirection. s->a is field .0 of the struct s points
// to.
void single_ptr(struct S *s) { s->a = 1; }

// (*s)->a: the struct is one load further away than in single_ptr, so the
// access path has to say "field .0 of *s", not "field .0 of s".
void double_ptr_field(struct S **s) { (*s)->a = 1; }

// Out-parameter: writes *s itself, then a field of the new object.
void double_ptr_alloc(struct S **s) {
  *s = malloc(sizeof(struct S));
  (*s)->b = 2;
}

// Array of struct pointers: s[i] is an array access, s[i]->a a field of the
// element's pointee.
void double_ptr_array(struct S **s, int i) { s[i]->a = 1; }

int main() {
  struct S x;
  struct S *px = &x;
  single_ptr(px);
  double_ptr_field(&px);
  double_ptr_alloc(&px);
  double_ptr_array(&px, 0);
  return 0;
}
