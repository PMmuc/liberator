llvm_map_components_to_libnames(llvm_libs bitwriter core ipo irreader instcombine instrumentation target linker analysis scalaropts support )
file (GLOB SOURCES
   *.cpp
)
add_executable(extractor ${SOURCES})

target_link_libraries(extractor ${SVF_LIB} ${llvm_libs})
target_link_libraries(extractor ${Z3_LIBRARIES})
set_target_properties(extractor PROPERTIES
                       RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin )
if(ENABLE_GPROF)
    message(STATUS "GProf enabled for target extractor!")
    set(PROFILING_FLAGS "-pg" "-O3" "-g" "-fno-omit-frame-pointer")
    target_compile_options(extractor PRIVATE ${PROFILING_FLAGS})
    target_link_options(extractor PUBLIC -pg)
endif()
