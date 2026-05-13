cmake_minimum_required(VERSION 3.20)
project(inferdeck_adlx_helper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(inferdeck-adlx-helper inferdeck_adlx_helper.cpp)

if(DEFINED ADLX_SDK_DIR)
  target_compile_definitions(inferdeck-adlx-helper PRIVATE INFERDECK_WITH_ADLX=1)
  target_include_directories(inferdeck-adlx-helper PRIVATE "${ADLX_SDK_DIR}/SDK/ADLXHelper/Windows" "${ADLX_SDK_DIR}/SDK/Include")
  target_link_directories(inferdeck-adlx-helper PRIVATE "${ADLX_SDK_DIR}/SDK/Lib/x64")
  target_link_libraries(inferdeck-adlx-helper PRIVATE ADLX)
endif()
