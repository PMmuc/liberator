REPO_URL="https://github.com/mm2/Little-CMS.git"
REPO_COMMIT="2daf5c5859e1b62b6633ca755074e4de02459241"
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

