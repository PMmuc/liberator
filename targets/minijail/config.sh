REPO_URL="https://chromium.googlesource.com/chromiumos/platform/minijail"
REPO_COMMIT="99e8fd4bf9aaf62eab9b3cabddc2939cb3427029"
BC_FILE_NAME="libminijail.pie.a"

target_configure() {
    # Minijail doesn't seem to have a configure step, just make
    true
}

target_build() {
    export CFLAGS="$CFLAGS -Wno-unused-command-line-argument -g -O0"
    export CXXFLAGS="$CXXFLAGS -Wno-unused-command-line-argument -g -O0"
    
    make -j$(nproc) clean
    make -j$(nproc) OUT="$WORK/lib" "CC_STATIC_LIBRARY(libminijail.pie.a)"
}

target_install() {
    # Headers are copied manually in original script
    cp *.h "$WORK/include"
    # Library was built into $WORK/lib by make command above
}

target_preinstall() {
    
}


target_preinstall_docker() {
    sudo apt-get update
        sudo apt-get install -y git make libcap-dev
}

