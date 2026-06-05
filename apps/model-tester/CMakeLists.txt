cmake_minimum_required(VERSION 3.27)
project(model_tester LANGUAGES CXX)

add_executable(inferdeck-model-tester
    main.cpp
)

target_link_libraries(inferdeck-model-tester PRIVATE
    model
    foundation
)

set_target_properties(inferdeck-model-tester PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(inferdeck-model-tester PRIVATE /W4 /utf-8 /FS)
else()
    target_compile_options(inferdeck-model-tester PRIVATE -Wall -Wextra -Wpedantic)
endif()
