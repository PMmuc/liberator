struct MyStruct {
    int id;
    char *buffer;
    int buffer_len;
};

extern void external_sink(char *buf);

int test_parameter_metadata(struct MyStruct *param1, int *param2, int len, void *param3) {
    // Test 1: GEP + Load (Read field)
    int local_id = param1->id;

    // Test 2: GEP + Store (Write field)
    param1->buffer_len = len;

    // Test 3: Array indexing in a loop dependency
    for (int i = 0; i < len; i++) {
        param2[i] = local_id + i;
    }

    // Test 4: Bitcast
    char *casted = (char*) param3;
    casted[0] = 'A';

    // Test 5: Parameter passed to another function
    external_sink(param1->buffer);

    return local_id;
}

int main() { return 0; }
