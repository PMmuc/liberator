cmake_minimum_required(VERSION 3.15)
project("Condition Extractor")

# 1. Add the option (Default is OFF)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_GPROF "Enable gprof profiling" OFF)

  # We need to match the build environment for LLVM:
  # In particular, we need the -fno-rtti flag
  set(CMAKE_CXX_STANDARD 23)
  if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(CMAKE_CXX_FLAGS "-fPIC -std=gnu++23 -O0 -Wno-deprecated")
  else()
  set(CMAKE_CXX_FLAGS "-fPIC -std=gnu++23 -O0 -Wno-deprecated")
  endif()

  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")

  set(SVF_WARN_AS_ERROR OFF CACHE BOOL "Disable Werror for SVF" FORCE)
  find_package(SVF REQUIRED CONFIG)
  find_package(Z3 REQUIRED CONFIG)
  #find_package(spdlog REQUIRED)

  message(STATUS "Use llvm-config: ${LLVM_CONFIG}")
  execute_process(
    COMMAND ${LLVM_CONFIG} --cmakedir
    OUTPUT_VARIABLE LLVM_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  message(STATUS "LLVM CMake Dir: ${LLVM_CMAKE_DIR}")

# LLVM finden
  find_package(LLVM REQUIRED CONFIG
    PATHS ${LLVM_CMAKE_DIR}
    NO_DEFAULT_PATH)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

  # set condition extractor to use llvm dynamically
  # SVF needs to be build linking LLVM dynamically or as a static library
  set(LLVM_LINK_LLVM_DYLIB ON)
  set(LLVM_USE_SHARED_LLVM ON)
  add_definitions(${LLVM_DEFINITIONS})
  include_directories(${SVF_INCLUDE_DIRS})
  link_directories(${LLVM_LIBRARY_DIRS})

  if(NOT ${LLVM_ENABLE_RTTI})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
  endif()

  include(AddLLVM)

  include(CheckIPOSupported)
  check_ipo_supported(RESULT lto_ok)

include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.4.0 # or a later release
)
FetchContent_MakeAvailable(Catch2)

enable_testing()

set(ASSETS_DIR "${CMAKE_SOURCE_DIR}/tests/assets")
configure_file(
  "${CMAKE_SOURCE_DIR}/src/Config.h.in"
  "${CMAKE_SOURCE_DIR}/src/Config.h"
)
include_directories("${CMAKE_BINARY_DIR}/include")

add_subdirectory(src)
add_subdirectory(tests)
