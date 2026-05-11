REPO_URL="https://github.com/madler/zlib"
# v1.3.2
REPO_COMMIT="da607da739fa6047df13e66a2af6b8bec7c2a498"
BC_FILE_NAME="libz.a"

target_configure() {
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
  APIS="adler32_z adler32 crc32 crc32_z gzwrite gzread"
  for a in ${APIS}; do
    "$TOOLS_DIR/tool/misc/edit_constraints_2.py" -v "param_1=>param_2" \
      -f ${a} -c "$LIBFUZZ_LOG_PATH/conditions.json"
  done
}

target_preinstall() {
  :
}

target_preinstall_docker() {
  :
}
