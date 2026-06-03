REPO_URL="https://github.com/libsndfile/libsndfile.git"
# v1.2.2
REPO_COMMIT="0d3f80b7394368623df558d8ba3fee6348584d4d"
BC_FILE_NAME="libsndfile.a"

target_configure() {
  mkdir -p "$TARGET/repo/libsndfile_build"
  cd "$TARGET/repo/libsndfile_build"

  cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_PROGRAMS=OFF -DBUILD_TESTING=OFF \
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

target_preinstall() { return 0; }


target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends autoconf autogen automake build-essential libasound2-dev libflac-dev libogg-dev libtool libvorbis-dev libopus-dev libmp3lame-dev libmpg123-dev pkg-config python libogg0 libopus0 libflac8 libmp3lame0 libasound2 libvorbis0a libmpg123-0
}
