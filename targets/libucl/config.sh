REPO_URL="https://github.com/vstakhov/libucl.git"
REPO_COMMIT="5c58d0d5b939daf6f0c389e15019319f138636c2"
BC_FILE_NAME="libucl.a"

target_configure() {
    ./autogen.sh
    ./configure --disable-shared --prefix="$WORK" \
        CC=wllvm CXX=wllvm++ \
        CXXFLAGS="-g -O0" \
        CFLAGS="-g -O0"

    # WATCH OUT PADAWAN! SOMETIME SETTING -O0 IN C{XX}FLAGS MIGHT NOT BE ENOUGH!
    find . -name Makefile -exec sed -i 's/-O2/-O0/g' {} \;
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

