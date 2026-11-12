cmake_minimum_required(VERSION 3.27)

add_library(decision_runtime STATIC
    src/jev_style_render.cpp
    src/jev_style_backend.cpp
)
target_include_directories(decision_runtime
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_SOURCE_DIR}/libs/third_party/llama.cpp/include)
target_link_libraries(decision_runtime
    PUBLIC model foundation
    PRIVATE llama nlohmann_json::nlohmann_json)
target_compile_features(decision_runtime PRIVATE cxx_std_23)
if(MSVC)
    target_compile_options(decision_runtime PRIVATE /W4 /utf-8 /FS)
endif()

if(INFERDECK_BUILD_TESTS)
    add_executable(decision_runtime_tests tests/test_jev_style_render.cpp src/jev_style_render.cpp)
    target_include_directories(decision_runtime_tests PRIVATE include)
    target_link_libraries(decision_runtime_tests PRIVATE model foundation Catch2::Catch2WithMain)
    target_compile_features(decision_runtime_tests PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(decision_runtime_tests PRIVATE /W4 /utf-8 /FS)
    endif()
    add_test(NAME decision_runtime_tests COMMAND decision_runtime_tests)
    set_tests_properties(decision_runtime_tests PROPERTIES LABELS "unit")
endif()
