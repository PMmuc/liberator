REPO_URL="https://github.com/sqlite/sqlite.git"
# 3.45.1
REPO_COMMIT="189e44dfecdc7868bb860dfb5d98eab371318c37"
BC_FILE_NAME="libsqlite3.a"

target_configure() {
  ./configure --disable-shared --prefix="$WORK" \
    CC=wllvm CXX=wllvm++ \
    CXXFLAGS="-g -O0" \
    CFLAGS="-g -O0"

  # SOMETIME SETTING -O0 IN C{XX}FLAGS MIGHT NOT BE ENOUGH!
  find . -name Makefile -exec sed -i 's/-O2/-O0/g' {} \;
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
  sudo apt-get -y install --no-install-suggests --no-install-recommends tclsh
}
