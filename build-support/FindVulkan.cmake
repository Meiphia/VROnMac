# OXRSys deliberately uses only Vulkan headers and resolves loader symbols at runtime.
find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h REQUIRED)
set(Vulkan_INCLUDE_DIRS "${Vulkan_INCLUDE_DIR}")
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Vulkan REQUIRED_VARS Vulkan_INCLUDE_DIR)
