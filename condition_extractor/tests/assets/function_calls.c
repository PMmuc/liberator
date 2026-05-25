int add(int a, int b) {
    return a + b;
}

int sub(int a, int b) {
    return a - b;
}

int main() {
    int x = 10, y = 5;
    int s = add(x, y);
    
    int (*op)(int, int) = sub;
    int d = op(x, y);
    
    return s + d;
}
