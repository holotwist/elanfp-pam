cmake_minimum_required(VERSION 3.16)
project(elanfp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(PORTABLE_BUILD "Compile for generic baseline architecture" OFF)
option(SEMI_STATIC "Link dependencies statically where available" OFF)
option(MODERN_X86_64 "Build for modern x86_64 level" OFF)
set(X86_64_LEVEL "x86-64-v3" CACHE STRING "Target x86-64 microarchitecture level")

# Compiler optimization flags
if(MODERN_X86_64)
    add_compile_options(-march=${X86_64_LEVEL})
elseif(PORTABLE_BUILD)
    add_compile_options(-mtune=generic)
else()
    add_compile_options(-march=native)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall -Wextra)
else()
    add_compile_options(-O3 -Wall -Wextra)
endif()

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
pkg_check_modules(SYSTEMD REQUIRED libsystemd)
find_library(MATH_LIB m REQUIRED)
find_package(Threads REQUIRED)

# ONNX Runtime detection
find_path(ONNX_INCLUDE_DIR onnxruntime_c_api.h PATHS ${CMAKE_CURRENT_SOURCE_DIR}/onnx/include /usr/include /usr/local/include)
find_library(ONNX_LIB onnxruntime PATHS ${CMAKE_CURRENT_SOURCE_DIR}/onnx/lib /usr/lib /usr/local/lib /usr/lib/elanfp)

if(NOT ONNX_INCLUDE_DIR OR NOT ONNX_LIB)
    message(FATAL_ERROR "ONNX Runtime not found. Place headers in ./onnx/include and libs in ./onnx/lib or install system-wide.")
endif()

include_directories(src ${LIBUSB_INCLUDE_DIRS} ${SYSTEMD_INCLUDE_DIRS} ${ONNX_INCLUDE_DIR})

add_executable(elanfpd
    src/elanfpd.c
    src/device.c
    src/storage.c
    src/elan_drv.c
    src/image.c
    src/matcher.c
    src/quality.c
    src/enroll_policy.c
)

target_link_libraries(elanfpd PRIVATE
    ${LIBUSB_LIBRARIES}
    ${SYSTEMD_LIBRARIES}
    ${ONNX_LIB}
    ${MATH_LIB}
    Threads::Threads
)

set_target_properties(elanfpd PROPERTIES
    BUILD_RPATH "\$ORIGIN/../lib/elanfp;/usr/lib/elanfp"
    INSTALL_RPATH "/usr/lib/elanfp"
)

include(GNUInstallDirs)
install(TARGETS elanfpd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})