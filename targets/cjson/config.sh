REPO_URL="https://github.com/DaveGamble/cJSON.git"
REPO_COMMIT="324973008ced4ea03d1626a00915d0399ecbd9db"
BC_FILE_NAME="libcjson.a"

target_configure() {
  mkdir -p "$TARGET/repo/cJSON_build"
  cd "$TARGET/repo/cJSON_build"

  cmake .. -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_AND_STATIC_LIBS=On \
    -DBUILD_SHARED_LIBS=off -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0"
}

target_build() {
  # We are already in build dir from configure step
  make -j"$(nproc)" clean
  make -j"$(nproc)"
}

target_install() {
  make install
}

target_preinstall() {
  sed -i "s/cmake_minimum_required(VERSION 3.0)/cmake_minimum_required(VERSION 3.5)/" CMakeLists.txt
}

target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends cmake git
}
