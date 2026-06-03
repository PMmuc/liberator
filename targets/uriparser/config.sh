REPO_URL="https://github.com/uriparser/uriparser.git"
# v1.0.2
REPO_COMMIT="9b2bed92f5deecf740819f9bf27724bee2fe9c12"
BC_FILE_NAME="liburiparser.a"

target_configure() {
  cmake . -DCMAKE_INSTALL_PREFIX=$WORK -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0"
}

target_build() {
  make -j$(nproc) clean
  make -j$(nproc)
}

target_install() {
  make install
}

target_preinstall() { return 0; }


target_preinstall_docker() {
  sudo apt-get update
  sudo apt-get install -y libgtest-dev doxygen
}
