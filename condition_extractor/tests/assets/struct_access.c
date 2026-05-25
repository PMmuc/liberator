typedef struct List_ {
  struct List_ *next;
} List;

typedef struct {
  int x;
  int y;
  List list;
} Point;

void test_func(Point fun_p) {
  List l;
  fun_p.list.next = &l;
}

int main() {
  Point p;
  p.x = 10;
  p.y = 20;
  test_func(p);

  return 0;
}
