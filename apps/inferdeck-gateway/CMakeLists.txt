cmake_minimum_required(VERSION 3.27)
project(inferdeck_gateway LANGUAGES CXX)

add_executable(inferdeck-gateway
    src/main.cpp
)

find_package(yaml-cpp REQUIRED)

target_link_libraries(inferdeck-gateway PRIVATE
    gateway
    model
    scheduler
    engine
    foundation
    llama_cpp_wrapper
    llama
    yaml-cpp::yaml-cpp
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(inferdeck-gateway PRIVATE dbghelp)
    target_link_options(inferdeck-gateway PRIVATE /STACK:33554432)
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
