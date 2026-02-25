REPO_URL="https://android.googlesource.com/platform/external/pthreadpool"
REPO_COMMIT="f355e616e15b366dae115c916ef19e3b70327ad5"
BC_FILE_NAME="libpthreadpool.a"

target_configure() {
    cmake . -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
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

target_preinstall() {
    :
}

target_preinstall_docker() {
    :
}

