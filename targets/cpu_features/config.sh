REPO_URL="https://android.googlesource.com/platform/external/cpu_features"
# android-vts-15.0_r8
REPO_COMMIT="eca53ba6d2e951e174b64682eaf56a36b8204c89"
BC_FILE_NAME="libcpu_features.a"

# cpu_features script used /usr/bin for LLVM_COMPILER_PATH but default is usually fine?
# We'll stick to default unless issues arise.

target_configure() {
  cmake . -DCMAKE_INSTALL_PREFIX=$WORK -DBUILD_SHARED_LIBS=off \
    -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS_DEBUG="-g -O0" \
    -DCMAKE_CXX_FLAGS_DEBUG="-g -O0"
}

target_build() {
  make -j$(nproc) clean
  make -j$(nproc) VERBOSE=1
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
