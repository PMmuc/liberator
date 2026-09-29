struct S {
  int *f1;
  int *f2;
};
struct S g[4];
int x, y;

int *foo(struct S *s) {
  int *i = s->f2;
  return s->f1;
}

int main() {
  g[0].f1 = &x;
  g[2].f1 = &y;
  g[3].f1 = &x;
  int *p = g[0].f1;
  int *q = foo(&g[3]);
  return *p;
}
