REPO_URL=""
REPO_COMMIT="d477edd"
BC_FILE_NAME="libnetwork_lib.a"

git() {
  if [ "$1" = "fetch" ]; then return 0; fi
  if [ "$1" = "clone" ]; then return 0; fi
  command git "$@"
}

target_configure() { :; }
target_build() {
  wllvm++ -c -o network_lib.o network_lib.cpp
  ar rcs libnetwork_lib.a network_lib.o
}
target_install() {
  mkdir -p "$WORK/lib" "$WORK/include"
  cp libnetwork_lib.a "$WORK/lib/"
  cp network_lib.hpp "$WORK/include/"
}
target_preinstall() { return 0; }
target_preinstall_docker() { :; }
