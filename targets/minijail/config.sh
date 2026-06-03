REPO_URL="https://chromium.googlesource.com/chromiumos/platform/minijail"
#v2025.07.02
REPO_COMMIT="b0b5dc6997956cdc027295ade5e425c416f30d33"
BC_FILE_NAME="libminijail.pie.a"

target_configure() {
  sed -i 's/(SIGRTMAX|SIGRTMIN|SIG_|NULL)/(SIGRTMAX|SIGRTMIN|SIG_|NULL|BLKTRACESETUP2)/g' "$TARGET/repo/gen_constants.sh"
}

target_build() {
  export CFLAGS="$CFLAGS -Wno-unused-command-line-argument -g -O0"
  export CXXFLAGS="$CXXFLAGS -Wno-unused-command-line-argument -g -O0"

  make -j$(nproc) clean
  make -j$(nproc) OUT="$WORK/lib" "CC_STATIC_LIBRARY(libminijail.pie.a)"
}

target_install() {
  # Headers are copied manually in original script
  mkdir -p "$WORK/include"
  cp *.h "$WORK/include"
  # Library was built into $WORK/lib by make command above
}

target_preinstall() { return 0; }


target_preinstall_docker() {
  sudo apt-get update
  sudo apt-get install -y git make libcap-dev
}
