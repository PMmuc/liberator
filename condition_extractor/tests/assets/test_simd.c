#include <emmintrin.h>

void process_simd(__m128i *dst, const __m128i *src) {
    // This assignment generates a load/store of a VectorType. 
    // SVF translates this into a memory copy operation.
    *dst = *src;
}

int main() {
    __m128i a, b;
    process_simd(&a, &b);
    return 0;
}
