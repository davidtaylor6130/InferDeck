cmake_minimum_required(VERSION 3.27)
project(inference LANGUAGES CXX)

add_library(inference INTERFACE)

target_include_directories(inference
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(inference INTERFACE cxx_std_23)

if(INFERDECK_BUILD_TESTS)
    add_subdirectory(tests)
endif()
