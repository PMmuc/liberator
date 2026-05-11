REPO_URL="https://github.com/mm2/Little-CMS.git"
#2.19.1
REPO_COMMIT="21c582a594fe5279f90c0b93437c398f93bf62b0"
BC_FILE_NAME="liblcms2.a"

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
}

target_preinstall() {
  :
}

target_preinstall_docker() {
  :
}
