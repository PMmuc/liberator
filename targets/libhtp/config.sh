REPO_URL="https://github.com/OISF/libhtp.git"
REPO_COMMIT="202be0f21622352fc3955efaa4112b2fec304dc7"
BC_FILE_NAME="libhtp.a"

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

