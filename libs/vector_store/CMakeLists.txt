# libs/vector_store CMakeLists.txt
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
    SQLite::SQLite3
)

target_compile_options(vector_store PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    --coverage
)
