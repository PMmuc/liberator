
enable_testing()

# Configure the header file to pass the test assets directory
configure_file(config.h.in config.h @ONLY)

# Compile .c files to .bc files
set(TESTS_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(BUILD_ASSETS_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

file(MAKE_DIRECTORY "${BUILD_ASSETS_DIR}")
file(COPY "${TESTS_ASSETS_DIR}/extapi.bc" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

file(GLOB C_TEST_FILES "${TESTS_ASSETS_DIR}/*.c")
foreach(c_file ${C_TEST_FILES})
    get_filename_component(basename ${c_file} NAME_WE)
    set(bc_file "${BUILD_ASSETS_DIR}/${basename}.bc")
    
    add_custom_command(
        OUTPUT ${bc_file}
        COMMAND clang -c -emit-llvm -g -O0 "${c_file}" -o "${bc_file}"
        DEPENDS "${c_file}"
        COMMENT "Compiling ${basename}.c to bitcode"
    )
    list(APPEND BC_FILES ${bc_file})
endforeach()

file(GLOB CXX_TEST_FILES "${TESTS_ASSETS_DIR}/*.cpp")
foreach(cxx_file ${CXX_TEST_FILES})
    get_filename_component(basename ${cxx_file} NAME_WE)
    set(bc_file "${BUILD_ASSETS_DIR}/${basename}.bc")
    
    add_custom_command(
        OUTPUT ${bc_file}
        COMMAND clang++ -c -emit-llvm -g -O0 "${cxx_file}" -o "${bc_file}"
        DEPENDS "${cxx_file}"
        COMMENT "Compiling ${basename}.cpp to bitcode"
    )
    list(APPEND BC_FILES ${bc_file})
endforeach()

add_custom_target(compile_test_assets ALL DEPENDS ${BC_FILES})

add_executable(unit_tests test_main.cpp)
add_dependencies(unit_tests compile_test_assets)

target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain condition_extractor_core)

# Ensure unit tests can include headers from src and the generated config.h
target_include_directories(unit_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/src 
    ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME IntegrationTests COMMAND unit_tests "Condition Extraction on .ll files")
add_test(NAME ParameterExtractTest COMMAND unit_tests "extractParameterMetadata standalone test")
