cmake_minimum_required(VERSION 3.20)
project(inferdeck_adlx_helper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ADLX_SDK_DIR)
  add_executable(inferdeck-adlx-helper
    inferdeck_adlx_helper.cpp
    "${ADLX_SDK_DIR}/SDK/ADLXHelper/Windows/Cpp/ADLXHelper.cpp"
    "${ADLX_SDK_DIR}/SDK/Platform/Windows/WinAPIs.cpp"
  )
  target_compile_definitions(inferdeck-adlx-helper PRIVATE INFERDECK_WITH_ADLX=1)
  target_include_directories(inferdeck-adlx-helper PRIVATE "${ADLX_SDK_DIR}" "${ADLX_SDK_DIR}/SDK/ADLXHelper/Windows/Cpp" "${ADLX_SDK_DIR}/SDK/Include")
else()
  add_executable(inferdeck-adlx-helper inferdeck_adlx_helper.cpp)
endif()
