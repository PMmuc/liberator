REPO_URL="https://github.com/libimobiledevice/libplist.git"
# v1.3
REPO_COMMIT="02cf35bb445ad1a6ed6180f78cfb6528a1e36c19"
BC_FILE_NAME="libplist.a"

target_configure() {
  export CXX=g++
  find . -name CMakeLists.txt -exec sed -i 's/SHARED//g' {} \;
  rm -rf "$TARGET/repo/build"
  mkdir -p "$TARGET/repo/build"
  cd "$TARGET/repo/build"
  cmake .. -DCMAKE_CXX_COMPILER=g++ -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS_DEBUG="-g -O0" -DCMAKE_CXX_FLAGS_DEBUG="-g -O0" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
}

target_build() {
  cd "$TARGET/repo/build"
  make -j$(nproc) clean
  make -j$(nproc)
}

target_install() {
  cd "$TARGET/repo/build"
  make install
}

target_preinstall() { return 0; }


target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends build-essential checkinstall git autoconf automake libtool-bin autopoint libdw-dev flex gawk cython3 cython
}
