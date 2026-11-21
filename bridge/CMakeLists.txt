cmake_minimum_required(VERSION 3.20)
project(vrconmac_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SOURCES
  src/main.cpp
  src/server.cpp
  src/backend_mock.cpp
  src/backend_openxr.cpp
  src/xr_loader.cpp
)

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND SOURCES src/gfx_metal.mm)
  set_source_files_properties(src/gfx_metal.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

add_executable(vrconmac-bridge ${SOURCES})
target_include_directories(vrconmac-bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
target_compile_options(vrconmac-bridge PRIVATE -Wall -Wno-missing-field-initializers)

find_package(Threads REQUIRED)
target_link_libraries(vrconmac-bridge PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(vrconmac-bridge PRIVATE "-framework Metal" "-framework Foundation")
endif()
