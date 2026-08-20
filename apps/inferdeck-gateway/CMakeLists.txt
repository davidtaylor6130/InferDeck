cmake_minimum_required(VERSION 3.27)
project(inferdeck_gateway LANGUAGES CXX)

add_executable(inferdeck-gateway
    src/main.cpp
)

find_package(yaml-cpp REQUIRED)

target_link_libraries(inferdeck-gateway PRIVATE
    gateway
    model
    foundation
    llama_cpp_wrapper
    native_runtimes
    llama
    yaml-cpp::yaml-cpp
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(inferdeck-gateway PRIVATE dbghelp)
    target_link_options(inferdeck-gateway PRIVATE /STACK:33554432)
    if(INFERDECK_SHERPA_ONNX_INCLUDE AND INFERDECK_SHERPA_ONNX_LIBRARY)
        add_custom_command(TARGET inferdeck-gateway POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${INFERDECK_SHERPA_ONNX_RUNTIME_DIR}/sherpa-onnx-c-api.dll"
                    "${INFERDECK_SHERPA_ONNX_RUNTIME_DIR}/onnxruntime.dll"
                    "${INFERDECK_SHERPA_ONNX_RUNTIME_DIR}/onnxruntime_providers_shared.dll"
                    "$<TARGET_FILE_DIR:inferdeck-gateway>"
            COMMENT "Copying sherpa-onnx runtime libraries")
    endif()
endif()

set_target_properties(inferdeck-gateway PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(inferdeck-gateway PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(inferdeck-gateway PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(INFERDECK_DASHBOARD_STATIC_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/static"
    CACHE PATH "Built InferDeck dashboard static asset directory")

add_custom_command(TARGET inferdeck-gateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_FILE_DIR:inferdeck-gateway>/static"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:inferdeck-gateway>/static"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${INFERDECK_DASHBOARD_STATIC_DIR}"
            "$<TARGET_FILE_DIR:inferdeck-gateway>/static"
    COMMENT "Copying InferDeck dashboard static assets"
)

if(INFERDECK_BUILD_TESTS)
    add_executable(config_tests tests/test_config.cpp)
    target_include_directories(config_tests PRIVATE src)
    target_compile_definitions(config_tests PRIVATE
        INFERDECK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(config_tests PRIVATE model foundation yaml-cpp::yaml-cpp Catch2::Catch2WithMain)
    set_target_properties(config_tests PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
    add_test(NAME config_tests COMMAND config_tests)
    set_tests_properties(config_tests PROPERTIES LABELS "unit")
    add_test(NAME lan_config_tests COMMAND config_tests "[lan]")
    set_tests_properties(lan_config_tests PROPERTIES LABELS "unit")
    add_test(NAME stats_path_config_tests COMMAND config_tests "[stats-path]")
    set_tests_properties(stats_path_config_tests PROPERTIES LABELS "unit")
    add_test(NAME tts_config_tests COMMAND config_tests "[speech-config]")
    set_tests_properties(tts_config_tests PROPERTIES LABELS "unit")
    if(WIN32)
        find_program(INFERDECK_POWERSHELL_EXECUTABLE powershell.exe REQUIRED)
        add_test(NAME control_security_local
            COMMAND "${INFERDECK_POWERSHELL_EXECUTABLE}" -NoProfile
                    -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/Testing/Test-ControlPlaneSecurity.ps1"
                    -Gateway "$<TARGET_FILE:inferdeck-gateway>")
        add_test(NAME control_security_remote
            COMMAND "${INFERDECK_POWERSHELL_EXECUTABLE}" -NoProfile
                    -ExecutionPolicy Bypass
                    -File "${CMAKE_SOURCE_DIR}/Testing/Test-ControlPlaneSecurity.ps1"
                    -Gateway "$<TARGET_FILE:inferdeck-gateway>"
                    -RemoteControl -TestSlowClient)
        set_tests_properties(control_security_local control_security_remote PROPERTIES
            LABELS "integration"
            TIMEOUT 90
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
endif()
