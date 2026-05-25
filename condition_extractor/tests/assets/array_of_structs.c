typedef struct {
  int x;
  int y;
} Point;

int test_fun(Point *points, int n) {
  int sum = 0;
  for (int i = 0; i < n; ++i) {
    sum += points[i].x + points[i].y;
  }
  return sum;
}

int main() {
  Point points[3];
  for (int i = 0; i < 3; ++i) {
    points[i].x = i;
    points[i].y = i * 2;
  }
  return test_fun(points, 3);
}
