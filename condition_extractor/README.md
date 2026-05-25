## 0. Preinstallation
```
pacman -S z3
yay -S gcc13
```

## 1. Download and build LLVM version 16
> We build LLVM with the older GCC version 13, because of incompatibility issues with the newer versions of GCC with the
SmallVector implementation in LLVM and other issues as well.

```
git clone --depth 1 --branch llvmorg-16.0.6 https://github.com/llvm/llvm-project
cd llvm-project && mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=<installation_path> -DLLVM_ENABLE_PROJECTS=clang -DBUILD_SHARED_LIBS=ON  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_OPTIMIZED_TABLEGEN=ON -DLLVM_TARGETS_TO_BUILD="X86" -DLLVM_ENABLE_RTTI=ON -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 ../llvm
ninja install
```

## 2. Download and build SVF version 3.2
```
git clone --depth 1 --branch v3.2 https://github.com/SVF-tools/SVF.git
cd SVF && mkdir build && cd build
cmake -G Ninja -DCMAKE_INSTALL_PREFIX=<installation_path> -DSVF_ENABLE_ASSERTIONS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSVF_WARN_AS_ERROR=OFF -DLLVM_LINK_LLVM_DYLIB=ON ..
ninja install
```

## 3. Build Condition Extractor
```
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLLVM_DIR=<llvm_dir path> ..
ninja
```

> If it can't link against LLVM make sure that LD_LIBRARY_PATH=<llvm_dir>/lib

## 4. Run Single Test
```
./tests/unit_tests "svf test <test_name>"
```

## 5. Run Profiling
Make sure that in the run_analysis.sh the -profiling flag is set, when invoking condition_extractor


