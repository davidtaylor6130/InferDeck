cmake_minimum_required(VERSION 3.27)

set(INTEGRATION_FIXTURES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures")

function(inferdeck_add_integration_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE
        foundation
        model
        Catch2::Catch2WithMain
    )
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /utf-8 /FS)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        LABELS "integration"
        ENVIRONMENT "INFERDECK_FIXTURES_DIR=${INTEGRATION_FIXTURES_DIR}"
    )
endfunction()

inferdeck_add_integration_test(integration_mocked_coordinator
    test_mocked_coordinator.cpp
)

if(MSVC AND DEFINED _VCPKG_INSTALLED_DIR AND EXISTS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/fmtd.dll")
    foreach(tgt integration_mocked_coordinator)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/fmtd.dll"
                "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/spdlogd.dll"
                $<TARGET_FILE_DIR:${tgt}>
            COMMENT "Copying vcpkg runtime DLLs for ${tgt}"
        )
    endforeach()
endif()
