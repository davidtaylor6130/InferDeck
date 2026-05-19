cmake_minimum_required(VERSION 3.27)
project(vector_store LANGUAGES CXX)

add_library(vector_store STATIC
    src/VectorStore.cpp
)

target_include_directories(vector_store PUBLIC
    include
)

target_link_libraries(vector_store PUBLIC
    core
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
)

if(MSVC)
    target_compile_options(vector_store PRIVATE /W4 /utf-8)
else()
    target_compile_options(vector_store PRIVATE -Wall -Wextra -Wpedantic)
endif()
