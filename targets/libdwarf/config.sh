REPO_URL="https://github.com/davea42/libdwarf-code.git"
# v2.3.1
REPO_COMMIT="b5ef10c9df0f494596fd9d31e19048a3ed5f28ba"
BC_FILE_NAME="libdwarf.a"

target_configure() {
  # In source build with cmake
  cmake . -DCMAKE_INSTALL_PREFIX=$WORK -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0" \
    -DBENCHMARK_ENABLE_GTEST_TESTS=off \
    -DBENCHMARK_ENABLE_INSTALL=off
}

target_build() {
  make -j$(nproc) clean
  make -j$(nproc)
}

target_install() {
  make install
}

target_post_analysis() {
  sed -i '/dwarf_register_printf_callback/d' "$LIBFUZZ_LOG_PATH/exported_functions.txt"
  sed -i '/dwarf_register_printf_callback/d' "$LIBFUZZ_LOG_PATH/apis_minimized.txt"
  sed -i '/dwarf_register_printf_callback/d' "$LIBFUZZ_LOG_PATH/apis_clang.json"
  sed -i '/dwarf_register_printf_callback/d' "$LIBFUZZ_LOG_PATH/apis_llvm.json"
  # Need to be careful with jq writing to temp file in current dir
  jq 'del(.[] | select(.function_name == "dwarf_register_printf_callback"))' "$LIBFUZZ_LOG_PATH/conditions.json" >"$LIBFUZZ_LOG_PATH/conditions.json.tmp" && mv "$LIBFUZZ_LOG_PATH/conditions.json.tmp" "$LIBFUZZ_LOG_PATH/conditions.json"
}

target_preinstall() { return 0; }

target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends pkgconf zlib1g zlib1g-dev libzstd1 libzstd-dev cmake
}
