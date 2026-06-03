REPO_URL="https://github.com/c-ares/c-ares.git"
# 1.34.6
REPO_COMMIT="3ac47ee46edd8ea40370222f91613fc16c434853"
BC_FILE_NAME="libcares.a"

target_configure() {
  mkdir -p "$TARGET/repo/cares_build"
  cd "$TARGET/repo/cares_build"

  cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug -DCARES_STATIC=on \
    -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0"
}

target_build() {
  # We are already in build dir from configure step
  make -j"$(nproc)" clean
  make -j"$(nproc)"
}

target_install() {
  make install
}

target_preinstall() {
  return 0
}

target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends cmake git
}
