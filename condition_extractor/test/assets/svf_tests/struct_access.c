struct Point {
    int x;
    int y;
};

int main() {
    struct Point p;
    p.x = 10;
    p.y = 20;
    
    struct Point *ptr = &p;
    int sum = ptr->x + ptr->y;
    
    return sum;
}
