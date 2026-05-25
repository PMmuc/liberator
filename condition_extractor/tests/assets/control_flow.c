int main() {
    int a = 10;
    int b = 20;
    if (a < b) {
        a = a + 1;
    } else {
        b = b + 1;
    }
    
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        sum += i;
    }
    
    return a + b + sum;
}
