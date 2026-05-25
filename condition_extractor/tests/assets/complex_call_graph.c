#include <stdlib.h>

// Struct definition
typedef struct {
    int id;
    float value;
} Data;

// Diverse return types
void void_func() {
    return;
}

int int_func(int x) {
    return x * 2;
}

float float_func(float x) {
    return x + 1.5f;
}

Data struct_func(int id, float val) {
    Data d;
    d.id = id;
    d.value = val;
    return d;
}

int* ptr_func(int* in) {
    return in;
}

// Inter-function calls (Chain)
int chain_C(int x) {
    return x + 1;
}

int chain_B(int x) {
    return chain_C(x) * 2;
}

int chain_A(int x) {
    return chain_B(x) - 5;
}

// Diamond Pattern
//      D_top
//     /     \
//  D_left  D_right
//     \     /
//     D_bottom

void D_bottom() {}

void D_left() {
    D_bottom();
}

void D_right() {
    D_bottom();
}

void D_top() {
    D_left();
    D_right();
}

// Recursion
int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main() {
    // Basic calls
    void_func();
    int i = int_func(10);
    float f = float_func(3.14f);
    
    Data d = struct_func(1, 2.5f);
    int* p = ptr_func(&i);

    // Call chain
    int chain_res = chain_A(10);

    // Diamond
    D_top();

    // Recursion
    int fact_res = factorial(5);

    return i + (int)f + d.id + *p + chain_res + fact_res;
}
