

int a = 0;
int b = 3;
int target_func(int cond, int *p) {
  if (cond) {
    p = &a;
  } else {
    p = &b;
  }

  return 0;
}

int main(int argc, char **argv) { return 0; }

// TODO: Look at how loops, data structures like lists
// traversing of these, simple pointers to struct,
// are converted to llvm 16 IR
// then find ways to find all read and writes to them.
