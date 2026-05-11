REPO_URL="https://github.com/OISF/libhtp.git"
# 0.5.53
REPO_COMMIT="16e23594c61f7719f8cb1cd19ca69bbafb37a0eb"
BC_FILE_NAME="libhtp.a"

target_configure() {
  ./autogen.sh
  ./configure --disable-shared --prefix="$WORK" \
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
  :
}

target_preinstall_docker() {
  :
}
