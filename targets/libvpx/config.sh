REPO_URL="https://chromium.googlesource.com/webm/libvpx"
# v.1.16.0
REPO_COMMIT="1024874c5919305883187e2953de8fcb4c3d7fa6"
BC_FILE_NAME="libvpx.a"

target_configure() {
  build_dir="$TARGET/build"
  rm -rf ${build_dir}
  mkdir -p ${build_dir}
  cd ${build_dir} # libvpx supports out-of-tree build

  export CXXFLAGS="-g -O0"
  export CFLAGS="-g -O0"

  # oss-fuzz has 2 GB total memory allocation limit. So, we limit per-allocation
  # limit in libvpx to 1 GB to avoid OOM errors. A smaller per-allocation is
  # needed for MemorySanitizer (see bug oss-fuzz:9497 and bug oss-fuzz:9499).
  if [[ $CFLAGS = *sanitize=memory* ]]; then
    extra_c_flags='-DVPX_MAX_ALLOCABLE_MEMORY=536870912'
  else
    extra_c_flags='-DVPX_MAX_ALLOCABLE_MEMORY=1073741824'
  fi

  # Using $REPO which is $TARGET/repo in analysis.sh
  LDFLAGS="$CXXFLAGS" LD=$CXX "$TARGET/repo/configure" \
    --prefix="$WORK" \
    --enable-vp9-highbitdepth \
    --disable-unit-tests \
    --disable-examples \
    --size-limit=12288x12288 \
    --extra-cflags="${extra_c_flags}" \
    --disable-webm-io \
    --enable-debug
}

target_build() {
  make -j all
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
