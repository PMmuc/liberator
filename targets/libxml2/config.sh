REPO_URL="https://github.com/GNOME/libxml2.git"
REPO_COMMIT="7846b0a677f8d3ce72486125fa281e92ac9970e8"
BC_FILE_NAME="libxml2.a"

target_configure() {
    ./autogen.sh
    ./configure --disable-shared --disable-docs --disable-tests \
        --prefix="$WORK" \
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

