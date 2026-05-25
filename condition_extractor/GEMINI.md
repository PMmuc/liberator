# Condition Extractor

`condition_extractor` is a specialized LLVM/SVF-based static analysis tool designed to extract constraints and value metadata from library bitcode (`.bc`) files. It identifies properties such as access types, array boundaries, malloc-size relationships, and file-path roles for the parameters and return values of exported API functions. The output is typically a JSON or text file that integrates into the larger `liberator` / `libfuzz` fuzzing pipeline.

## Build and Run

### Environment Setup
The project requires **LLVM 16**, **SVF 3.2**, and **Z3**. Environment variables `LLVM_DIR` and `SVF_DIR` must be set. An `env.sh` script is provided to automate this setup.

```bash
. ./env.sh
```

### Building
Use CMake and Ninja for building. The project requires C++23.

```bash
mkdir -p build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
ninja
```

**Common CMake Options:**
- `-DENABLE_ASAN=ON`: Enable AddressSanitizer.
- `-DENABLE_GPROF=ON`: Enable gprof profiling.
- `-DLLVM_DIR=<path>`: Explicitly set LLVM directory if not in environment.

### Running
The main binary is `extractor`.

**Analyze a single function:**
```bash
./bin/extractor <library.bc> -function <fn_name> -output out.json -t json -v v1
```

**Analyze a whole API:**
```bash
./bin/extractor <library.bc> -interface apis.json -output conditions.json -t json -do_indirect_jumps
```

**Common Flags:**
- `-v v0|v1|v2|v3`: Set verbosity level.
- `-log <tag1,tag2>`: Enable specific debug log tags (e.g., `Handler`, `GEPHandler`).
- `-do_indirect_jumps`: Include indirect calls in the analysis.
- `-profiling`: Enable time tracing and performance summary.

## Testing
The project uses **Catch2** for unit and integration testing, managed via CTest.

```bash
cd build
ctest --output-on-failure
```

**Directly running tests:**
```bash
./tests/unit_tests "svf test <test_name>"
```
Set `LIBERATOR_TEST_NO_FORK=1` when debugging with GDB to prevent the test runner from forking.

## Architecture and Development

### Key Components
- **`src/extractor.cpp`**: CLI entry point and configuration parsing.
- **`ConditionExtractor`**: Manages the SVF environment, pointer analysis, and the main extraction loop.
- **`AccessTypeHandlers`**: A set of handlers keyed by SVF node type that propagate metadata during graph traversal.
- **`ValueMetadata`**: Data structure holding the extracted properties for a specific value.
- **`GlobalStruct`**: Custom flow-sensitive pointer analysis built on top of SVF.

### Conventions
- **Language**: C++23.
- **Namespaces**: Primarily uses the `liberator` namespace.
- **Configuration**: Use the `config_t` singleton for process-wide settings.
- **Logging**: Use the `*_LOG` macros and `tag_log` system defined in `Config.h`.
- **Metadata**: When adding new analysis logic, implement it as a handler in `AccessTypeHandler.cpp`.
- **Code Style**: Adhere to existing patterns, which rely heavily on LLVM and SVF idioms. Use RAII for resource management and profiling.

### Configuration Files
- `src/Config.h.in`: Template for the generated `Config.h`. Do not edit `Config.h` directly.
- `CLAUDE.md`: Contains detailed technical notes and implementation details for the analyzer.
