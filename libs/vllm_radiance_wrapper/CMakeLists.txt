cmake_minimum_required(VERSION 3.27)

option(INFERDECK_VLLM_RADIANCE_ENABLE "Build the optional embedded CPython vLLM/Radiance runtime" OFF)
set(INFERDECK_PYTHON312_ROOT "" CACHE PATH "Embedded x64 CPython 3.12 root for vLLM/Radiance")

if(NOT INFERDECK_VLLM_RADIANCE_ENABLE)
    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR "vllm_radiance_wrapper is a Windows-only optional runtime")
endif()
if(NOT EXISTS "${INFERDECK_PYTHON312_ROOT}/include/Python.h" OR
   NOT EXISTS "${INFERDECK_PYTHON312_ROOT}/libs/python312.lib")
    message(FATAL_ERROR "INFERDECK_PYTHON312_ROOT must contain CPython 3.12 headers and python312.lib")
endif()

add_library(vllm_radiance_wrapper STATIC
    src/vllm_radiance_model.cpp
)
target_include_directories(vllm_radiance_wrapper
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE "${INFERDECK_PYTHON312_ROOT}/include"
)
target_link_libraries(vllm_radiance_wrapper PUBLIC model foundation inference
    PRIVATE "${INFERDECK_PYTHON312_ROOT}/libs/python312.lib")
set_target_properties(vllm_radiance_wrapper PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(vllm_radiance_wrapper PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(vllm_radiance_wrapper PRIVATE -Wall -Wextra -Wpedantic)
endif()
