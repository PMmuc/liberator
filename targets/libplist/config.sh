REPO_URL="https://github.com/libimobiledevice/libplist.git"
# v1.3
REPO_COMMIT="02cf35bb445ad1a6ed6180f78cfb6528a1e36c19"
BC_FILE_NAME="libplist-2.0.a"

target_configure() {
  ./autogen.sh --enable-debug --without-cython --with-tools=no --without-tests --prefix="$WORK" \
    CC=wllvm CXX=wllvm++ \
    CXXFLAGS="-g -O0" \
    CFLAGS="-g -O0"

  # WATCH OUT PADAWAN! SOMETIME SETTING -O0 IN C{XX}FLAGS MIGHT NOT BE ENOUGH!
  find . -name Makefile -exec sed -i 's/-O2/-O0/g' {} \;
}

target_build() {
  make -j$(nproc) clean
  make -j$(nproc)
}

target_install() {
  make install
}

target_preinstall() {

}

target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends build-essential checkinstall git autoconf automake libtool-bin autopoint libdw-dev flex gawk cython3 cython
}
