REPO_URL="https://github.com/GNOME/libxml2.git"

# v2.15.3
REPO_COMMIT="c94eb0210183b9d7cb43f8e7fddc6be55843ef49"
BC_FILE_NAME="libxml2.a"

target_configure() {
  ./autogen.sh
  ./configure --disable-shared --disable-docs --disable-tests \
    --prefix="$WORK" \
    CC=wllvm CXX=wllvm++ \
    CXXFLAGS="-g -O0" \
    CFLAGS="-g -O0"
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
