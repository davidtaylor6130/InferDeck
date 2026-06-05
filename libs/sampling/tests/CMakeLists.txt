cmake_minimum_required(VERSION 3.27)

add_executable(sampling_tests
    test_sampler.cpp
)

target_link_libraries(sampling_tests PRIVATE
    sampling
    Catch2::Catch2WithMain
)

set_target_properties(sampling_tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(sampling_tests PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(sampling_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MSVC AND DEFINED _VCPKG_INSTALLED_DIR AND EXISTS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/fmtd.dll")
    add_custom_command(TARGET sampling_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/fmtd.dll"
            "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/spdlogd.dll"
            $<TARGET_FILE_DIR:sampling_tests>
        COMMENT "Copying vcpkg runtime DLLs for sampling_tests"
    )
endif()

add_test(NAME sampling_tests COMMAND sampling_tests)
set_tests_properties(sampling_tests PROPERTIES LABELS "unit")
