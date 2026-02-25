
enable_testing()

add_executable(unit_tests test_main.cpp)
target_link_libraries(unit_tests PRIVATE Catch2::Catch2WithMain condition_extractor_core)

# Ensure unit tests can include headers from src
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

add_test(NAME unit_tests COMMAND unit_tests)
