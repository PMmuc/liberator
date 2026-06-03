typedef struct List_ {
  struct List_ *next;
} List;

typedef struct {
  int x;
  int y;
  List list;
} Point;

typedef Point *PointPtr;

void test_func(PointPtr fun_p) {
  List l;
  fun_p->list.next = &l;
}

// structs passed by value that are large should
// be bassed byval
struct Big {
  long a, b, c, d, e;
};
void test_func1(struct Big s) { return; }

// Function pointers
typedef int (*cb_t)(void *);
void test_func2(cb_t c) { return; }
void test_func3(void *v) { return; }

typedef int (*cb1_t)(int *, void *, float);

void test_func4(cb1_t *c) { return; }

typedef int (*cb2_t)(int **, void *, float);
void test_func5(cb2_t *u) {}

void test_func6(int **first) {}
