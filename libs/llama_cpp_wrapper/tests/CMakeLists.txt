cmake_minimum_required(VERSION 3.27)

find_package(Catch2 REQUIRED)

add_executable(llama_cpp_model_tests
    test_llama_cpp_model.cpp
)

target_link_libraries(llama_cpp_model_tests PRIVATE
    llama_cpp_wrapper
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_options(llama_cpp_model_tests PRIVATE /W4 /utf-8 /FS /bigobj)
    target_compile_definitions(llama_cpp_model_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    target_compile_options(llama_cpp_model_tests PRIVATE -Wall -Wextra)
endif()

include(CTest)
if(TARGET ggml-vulkan)
    add_executable(vulkan_idle_cache_tests test_vulkan_idle_cache.cpp)
    target_link_libraries(vulkan_idle_cache_tests PRIVATE ggml-vulkan ggml-base)
    add_test(NAME vulkan_idle_cache COMMAND vulkan_idle_cache_tests)
    set_tests_properties(vulkan_idle_cache PROPERTIES LABELS "integration;gpu" SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
include(Catch)
catch_discover_tests(llama_cpp_model_tests
    TEST_PREFIX "llama_"
    PROPERTIES LABELS "unit"
)
