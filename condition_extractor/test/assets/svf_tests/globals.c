int global_var = 100;

void modify() {
    global_var += 10;
}

int main() {
    modify();
    return global_var;
}
