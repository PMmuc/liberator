REPO_URL="https://github.com/cisco/openh264.git"
# v2.6.0
REPO_COMMIT="652bdb7719f30b52b08e506645a7322ff1b2cc6f"
BC_FILE_NAME="libopenh264.a"

target_configure() {
  # No configure script, pure makefile
  true
}

target_build() {
  make -j$(nproc) clean
  # install moves it to destdir, which includes build object
  # But analysis.sh calls target_build then target_install.
  # We can do the build and install in install step or split it.
  # Use make install as build step effectively
  make -j$(nproc) DESTDIR="$WORK" PREFIX="" OS=linux ARCH=x86_64 V=No install
}

target_install() {
  # Already done in build, or could be empty.
  true
}

target_preinstall() {

}

target_preinstall_docker() {
  sudo apt-get update
  sudo apt-get install -y git make libcap-dev
}
