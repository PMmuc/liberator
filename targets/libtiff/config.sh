REPO_URL="https://gitlab.com/libtiff/libtiff.git"
REPO_COMMIT="4e63559f2b7fa3ab5c8fa8ea0dbcc21e62286fe0"
BC_FILE_NAME="libtiff.a"

target_configure() {
  ./autogen.sh
  ./configure --disable-shared --prefix="$WORK" \
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
  # libtiff specific: extract extra archive although not strictly used by next step in original script,
  # we preserve it just in case.
  if [ -f "$WORK/lib/libtiffxx.a" ]; then
    extract-bc -b "$WORK/lib/libtiffxx.a"
  fi
}

target_preinstall() {
  :
}

target_preinstall_docker() {
  sudo apt-get update
  sudo apt-get install -y git make autoconf automake libtool cmake nasm zlib1g-dev liblzma-dev libjpeg-turbo8-dev wget
}
