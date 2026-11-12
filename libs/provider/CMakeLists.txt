cmake_minimum_required(VERSION 3.27)
project(provider LANGUAGES CXX)

add_library(provider STATIC src/plugin_provider.cpp)
target_include_directories(provider PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(provider
    PUBLIC model foundation
    PRIVATE nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
target_compile_features(provider PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(provider PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(provider PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Reference provider: the template for new providers and the test fixture.
# It links nothing from InferDeck; only the ABI header is shared.
add_library(inferdeck-provider-echo MODULE examples/echo_provider.cpp)
target_include_directories(inferdeck-provider-echo PRIVATE include)
target_link_libraries(inferdeck-provider-echo PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(inferdeck-provider-echo PRIVATE cxx_std_20)
set_target_properties(inferdeck-provider-echo PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/provider-examples"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/provider-examples")

if(INFERDECK_BUILD_TESTS)
    add_executable(provider_tests tests/test_plugin_provider.cpp)
    target_link_libraries(provider_tests PRIVATE
        provider model foundation nlohmann_json::nlohmann_json Catch2::Catch2WithMain)
    target_compile_features(provider_tests PRIVATE cxx_std_23)
    target_compile_definitions(provider_tests PRIVATE
        INFERDECK_ECHO_PROVIDER="$<TARGET_FILE:inferdeck-provider-echo>")
    add_dependencies(provider_tests inferdeck-provider-echo)
    if(MSVC)
        target_compile_options(provider_tests PRIVATE /W4 /utf-8 /FS)
    endif()
    add_test(NAME provider_tests COMMAND provider_tests)
    set_tests_properties(provider_tests PROPERTIES LABELS "unit")
endif()
