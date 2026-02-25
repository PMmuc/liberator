llvm_map_components_to_libnames(llvm_libs bitwriter core ipo irreader instcombine instrumentation target linker analysis scalaropts support )
file (GLOB SOURCES
   *.cpp
)
list(REMOVE_ITEM SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/extractor.cpp)

add_library(condition_extractor_core ${SOURCES})
target_link_libraries(condition_extractor_core PUBLIC SVF::SvfLLVM ${Z3_LIBRARIES})
#target_include_directories(condition_extractor_core PRIVATE $<TARGET_PROPERTY:SVF::SvfLLVM,INTERFACE_INCLUDE_DIRECTORIES>)

if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")

    # 2. Check for supported compilers (GCC/Clang)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        # 3. Apply flags to your target
        # Replace 'your_app_name' with the actual name of your executable/library
        target_compile_options(condition_extractor_core PRIVATE 
            -fsanitize=address 
            -fno-omit-frame-pointer 
            -g
        )
        target_link_options(condition_extractor_core PRIVATE 
            -fsanitize=address
        )
    else()
        message(WARNING "AddressSanitizer is not fully supported on this compiler via this simplified check.")
    endif()
else()
  message(STATUS "AddressSanitizer disabled")
endif()

add_executable(extractor extractor.cpp)

get_target_property(DEP_FLAGS SVF::SvfFlags INTERFACE_LINK_OPTIONS)
message(STATUS "lib_A flags: ${DEP_FLAGS}")

#target_link_libraries(extractor PRIVATE $<LINK_ONLY:SVF::SvfLLVM> ${llvm_libs} ${Z3_LIBRARIES})
target_link_libraries(extractor PRIVATE condition_extractor_core)
set_target_properties(extractor PROPERTIES
                       RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin )
get_target_property(LINK_LIBS extractor LINK_LIBRARIES)
message(STATUS ${LINK_LIBS})
list(FILTER LINK_LIBS EXCLUDE REGEX ".*LLVM.*\\.so.*")
set_target_properties(extractor PROPERTIES LINK_LIBRARIES "${LINK_LIBS}")


if(ENABLE_GPROF)
    message(STATUS "GProf enabled")
    target_compile_options(condition_extractor_core PUBLIC -pg)
    target_link_options(condition_extractor_core PUBLIC -pg)
    target_compile_options(extractor PUBLIC -pg)
    target_link_options(extractor PUBLIC -pg)
endif()
