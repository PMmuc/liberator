REPO_URL="https://github.com/sqlite/sqlite.git"
REPO_COMMIT="538ad6ce58c47e48f2c85abfcb31c968e615fc40"
BC_FILE_NAME="libsqlite3.a"

# SQLite specific: override LLVM_COMPILER_PATH if needed, though analysis.sh sets it to $LLVM_DIR/bin by default.
# The original script set it to /usr/bin but also used wllvm.
# We will trust the centralized script's default or environment overrides, but we can override here if strictly necessary.
# export LLVM_COMPILER_PATH=/usr/bin

target_configure() {
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
    
}


target_preinstall_docker() {
    sudo apt-get -y install --no-install-suggests --no-install-recommends tclsh
}

