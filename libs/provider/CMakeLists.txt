cmake_minimum_required(VERSION 3.27)
project(provider LANGUAGES CXX)

# Wire format shared by the gateway and every SDK provider (docs/providers.md).
add_library(provider_codec STATIC src/codec.cpp)
target_include_directories(provider_codec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(provider_codec PUBLIC model foundation nlohmann_json::nlohmann_json)
target_compile_features(provider_codec PUBLIC cxx_std_23)

add_library(provider STATIC src/plugin_provider.cpp)
target_include_directories(provider PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(provider
    PUBLIC model foundation
    PRIVATE provider_codec nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_features(provider PRIVATE cxx_std_23)

# Provider SDK: turns a C++ IBackend factory into a v2 provider DLL.
add_library(provider_sdk STATIC src/sdk.cpp)
target_link_libraries(provider_sdk PUBLIC provider_codec model foundation)
target_compile_features(provider_sdk PUBLIC cxx_std_23)
set_target_properties(provider_codec provider_sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)

foreach(target IN ITEMS provider_codec provider provider_sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /FS /bigobj)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# inferdeck_add_provider(<target> RUNTIME <name> SOURCES ... [LIBRARIES ...])
# Builds <name>.provider.dll from SDK sources into bin/<config>/providers, next to the gateway.
function(inferdeck_add_provider target)
    cmake_parse_arguments(ARG "" "RUNTIME;OUTPUT_DIRECTORY" "SOURCES;LIBRARIES" ${ARGN})
    if(NOT ARG_OUTPUT_DIRECTORY)
        set(ARG_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/providers")
        set(ARG_INSTALL ON)
    endif()
    add_library(${target} MODULE ${ARG_SOURCES})
    if(ARG_INSTALL)
        install(TARGETS ${target} LIBRARY DESTINATION providers RUNTIME DESTINATION providers)
    endif()
    target_link_libraries(${target} PRIVATE provider_sdk ${ARG_LIBRARIES})
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${ARG_RUNTIME}.provider"
        CXX_VISIBILITY_PRESET hidden
        RUNTIME_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}")
    foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config} "${ARG_OUTPUT_DIRECTORY}"
            LIBRARY_OUTPUT_DIRECTORY_${config} "${ARG_OUTPUT_DIRECTORY}")
    endforeach()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /FS)
    endif()
endfunction()

# Reference providers: v2 via the SDK (template for C++ runtimes) and the
# raw-C v1 table, which keeps v1 loading tested. v1 links only the header.
inferdeck_add_provider(inferdeck-provider-echo RUNTIME echo
    SOURCES examples/echo_provider_v2.cpp
    OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/provider-examples")
add_library(inferdeck-provider-echo-v1 MODULE examples/echo_provider.cpp)
target_include_directories(inferdeck-provider-echo-v1 PRIVATE include)
target_link_libraries(inferdeck-provider-echo-v1 PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(inferdeck-provider-echo-v1 PRIVATE cxx_std_20)
set_target_properties(inferdeck-provider-echo-v1 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "echo_v1.provider"
    CXX_VISIBILITY_PRESET hidden
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/provider-examples"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/provider-examples")

# Validates a provider library before it is installed. See docs/providers.md.
add_executable(inferdeck-provider-check tools/provider_check.cpp)
target_link_libraries(inferdeck-provider-check PRIVATE provider model foundation)
target_compile_features(inferdeck-provider-check PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(inferdeck-provider-check PRIVATE /W4 /utf-8 /FS)
endif()


if(INFERDECK_BUILD_TESTS)
    add_executable(provider_tests tests/test_plugin_provider.cpp)
    target_link_libraries(provider_tests PRIVATE
        provider provider_codec model foundation nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    target_compile_features(provider_tests PRIVATE cxx_std_23)
    target_compile_definitions(provider_tests PRIVATE
        INFERDECK_ECHO_PROVIDER="$<TARGET_FILE:inferdeck-provider-echo>"
        INFERDECK_ECHO_V1_PROVIDER="$<TARGET_FILE:inferdeck-provider-echo-v1>")
    add_dependencies(provider_tests inferdeck-provider-echo inferdeck-provider-echo-v1)
    if(MSVC)
        target_compile_options(provider_tests PRIVATE /W4 /utf-8 /FS)
    endif()
    add_test(NAME provider_tests COMMAND provider_tests)
    add_test(NAME provider_check_echo COMMAND inferdeck-provider-check
             $<TARGET_FILE:inferdeck-provider-echo>)
    add_test(NAME provider_check_echo_v1 COMMAND inferdeck-provider-check
             $<TARGET_FILE:inferdeck-provider-echo-v1>
             --artifact image=$<TARGET_FILE:inferdeck-provider-echo-v1>)
    set_tests_properties(provider_tests provider_check_echo provider_check_echo_v1 PROPERTIES LABELS "unit")
endif()
