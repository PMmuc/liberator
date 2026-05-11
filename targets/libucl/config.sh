REPO_URL="https://github.com/vstakhov/libucl.git"
# 0.9.4
REPO_COMMIT="058286f4f85e2a66130e8bdaddf402d9c78d259c"
BC_FILE_NAME="libucl.a"

target_configure() {
  ./autogen.sh
  ./configure --disable-shared --prefix="$WORK" \
    CC=wllvm CXX=wllvm++ \
    CXXFLAGS="-g -O0" \
    CFLAGS="-g -O0"

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
  :
}

target_preinstall_docker() {
  :
}
