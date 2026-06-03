REPO_URL="https://aomedia.googlesource.com/aom"
# v3.13.3
REPO_COMMIT="92d4c37fbdd08944a0e721bbaeb13318f10aebb0"
BC_FILE_NAME="libaom.a"

target_configure() {
  export CXX=g++
  rm -rf "$TARGET/repo/aom_build"
  mkdir -p "$TARGET/repo/aom_build"
  cd "$TARGET/repo/aom_build"

  cmake .. -DCMAKE_CXX_COMPILER=g++ -DCMAKE_INSTALL_PREFIX="$WORK" -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
    -DENABLE_TESTS=0 \
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

target_post_analysis() {
  # manual way to modify automatically extracted constraints
  TMP_FILE=/tmp/const.tmp

  NEW_FIELD_CONSTRAINT=$(
    cat <<EOF
{
    "access": "delete",
    "fields": [],
    "parent": 0,
    "type": "422ff5a78e8fa96c8729e7aaafcdcdf5",
    "type_string": "%struct.aom_codec_ctx*"
}
EOF
  )
  echo ${NEW_FIELD_CONSTRAINT} >${TMP_FILE}

  $TOOLS_DIR/tool/misc/edit_constraints.py -n ${TMP_FILE} \
    -f "aom_codec_destroy" -a 0 \
    -c "$LIBFUZZ_LOG_PATH/conditions.json"

  rm -f ${TMP_FILE}
}

target_preinstall() { return 0; }


target_preinstall_docker() {
  sudo apt-get -y install --no-install-suggests --no-install-recommends cmake git perl yasm
}
