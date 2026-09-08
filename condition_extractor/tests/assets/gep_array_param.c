// Exercises the is_array detection in the GEP handler
// (handleGep in src/AccessType.cpp).
//
//  - read_array indexes the pointer parameter like an array (a[i]) before
//    selecting a field -> the parameter summary must have is_array == true.
//  - read_field only selects constant struct fields (p->x, p->y) -> the
//    parameter summary must have is_array == false.
//
// Both parameters are struct pointers so the GEP source element type is a
// struct type (the supported field-sensitive path).

typedef struct {
  int x;
  int y;
} Point;

int read_array(Point *a, int n) {
  int sum = 0;
  for (int i = 0; i < n; i++)
    sum += a[i].x;
  return sum;
}

int read_field(Point *p) { return p->x + p->y; }

int main(void) {
  Point arr[4] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
  Point pt = {5, 6};
  return read_array(arr, 4) + read_field(&pt);
}
