
enable_testing()

# Configure the header file to pass the test assets directory
configure_file(config.h.in config.h @ONLY)

add_executable(unit_tests test_main.cpp)
target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain condition_extractor_core)

# Ensure unit tests can include headers from src and the generated config.h
target_include_directories(unit_tests PRIVATE 
    ${CMAKE_SOURCE_DIR}/src 
    ${CMAKE_CURRENT_BINARY_DIR}
)

add_test(NAME IntegrationTests COMMAND unit_tests "Condition Extraction on .ll files")
add_test(NAME ParameterExtractTest COMMAND unit_tests "extractParameterMetadata standalone test")
