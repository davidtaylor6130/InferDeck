find_path(Vulkan_INCLUDE_DIR
    NAMES vulkan/vulkan.h
    PATHS
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/third_party/Vulkan-Headers/include"
        "${CMAKE_CURRENT_LIST_DIR}/../libs/third_party/Vulkan-Headers/include"
    NO_DEFAULT_PATH
)

set(Vulkan_LIBRARY "C:/Windows/System32/vulkan-1.dll")

if(Vulkan_INCLUDE_DIR AND EXISTS "${Vulkan_LIBRARY}")
    set(Vulkan_FOUND TRUE)
    set(Vulkan_VERSION "1.3")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Vulkan
    REQUIRED_VARS Vulkan_LIBRARY Vulkan_INCLUDE_DIR
    VERSION_VAR Vulkan_VERSION
)

if(Vulkan_FOUND AND NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan SHARED IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${Vulkan_LIBRARY}"
        IMPORTED_IMPLIB "${Vulkan_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(Vulkan_INCLUDE_DIR Vulkan_LIBRARY)
