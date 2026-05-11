REPO_URL="https://github.com/the-tcpdump-group/libpcap.git"
# 1.10.6
REPO_COMMIT="a999701dca5c873779281938baee6bc185a8d4dc"
BC_FILE_NAME="libpcap.a"

target_configure() {
  mkdir -p "$TARGET/repo/pcap_build"
  cd "$TARGET/repo/pcap_build"

  cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0"
}

target_build() {
  make -j"$(nproc)" clean
  make -j"$(nproc)"
}

target_install() {
  make install
}

target_preinstall() {

}

target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends cmake git flex pkg-config
}
