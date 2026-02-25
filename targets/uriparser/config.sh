REPO_URL="https://github.com/uriparser/uriparser.git"
REPO_COMMIT="1762d5ff025fb07b4b8ccd1a8a9635009b2e9e34"
BC_FILE_NAME="liburiparser.a"

target_configure() {
    # Original script added -mllvm -get-api-pass to flags. 
    # Usually this is done via CFLAGS/CXXFLAGS export in analysis.sh?
    # No, analysis.sh sets defaults but doesn't include -get-api-pass.
    # But wait, other scripts commented out `export CFLAGS="-mllvm -get-api-pass"`.
    # Only uriparser script has it enabled in cmake flags.
    # If this is required for uriparser logic (maybe it uses it?), I should keep it.
    
    cmake . -DCMAKE_INSTALL_PREFIX=$WORK -DBUILD_SHARED_LIBS=off \
        -DENABLE_STATIC=on -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_C_FLAGS_DEBUG="-g -O0 -mllvm -get-api-pass" \
        -DCMAKE_CXX_FLAGS_DEBUG="-g -O0 -mllvm -get-api-pass" 
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
    sudo apt-get update
        sudo apt-get install -y libgtest-dev doxygen
}

