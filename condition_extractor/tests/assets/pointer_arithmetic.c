int main() {
    int arr[5] = {1, 2, 3, 4, 5};
    int *ptr = arr;
    int x = *(ptr + 2);
    int y = *(ptr + 4);
    return x + y;
}
