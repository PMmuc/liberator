REPO_URL="https://android.googlesource.com/platform/external/pthreadpool"
#android-vts-15.0_r8
REPO_COMMIT="3278138cd43f4d81aed2a406680f2dc328492d2e"
BC_FILE_NAME="libpthreadpool.a"

target_configure() {
  cmake . -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0" \
    -DBENCHMARK_ENABLE_GTEST_TESTS=off \
    -DBENCHMARK_ENABLE_INSTALL=off \
    -DPTHREADPOOL_BUILD_TESTS=OFF \
    -DPTHREADPOOL_BUILD_BENCHMARKS=OFF
}

target_build() {
  make -j$(nproc) clean
  make -j$(nproc)
}

target_install() {
  make install
}

target_preinstall() {
  :
}

target_preinstall_docker() {
  :
}
