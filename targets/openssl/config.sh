REPO_URL="https://github.com/openssl/openssl.git"
#4.0.0
REPO_COMMIT="3bd5319b5d0df9ecf05c8baba2c401ad8e3ba130"
BC_FILE_NAME="libssl.a"

# OpenSSL headers are installed into include/openssl
TARGET_INCLUDE_DIR="$WORK/include/openssl"

target_preinstall() {
  # OpenSSL target copied abilist.txt
  if [ -f "$TOOLS_DIR/targets/openssl/src/abilist.txt" ]; then
    cp "$TOOLS_DIR/targets/openssl/src/abilist.txt" "$TARGET/repo/abilist.txt"
  fi
}

target_configure() {
  # the config script supports env var LDLIBS instead of LIBS
  export LDLIBS="$LIBS"

  ./config --debug $CFLAGS -fno-sanitize=alignment $CONFIGURE_FLAGS --prefix="$WORK"
}

target_build() {
  export CXXFLAGS="-fPIC -DOPENSSL_PIC"
  make -j$(nproc) clean
  make -j$(nproc) LDCMD="$CXX $CXXFLAGS"
}

target_install() {
  make install
}

target_preinstall_docker() {
  sudo apt-get update
  sudo apt-get install -y git make autoconf automake libtool
}
