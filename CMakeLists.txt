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
find_library(PAM_LIB pam REQUIRED)
find_library(MATH_LIB m REQUIRED)

# ONNX Runtime detection
# local ./onnx first, then system
find_path(ONNX_INCLUDE_DIR onnxruntime_c_api.h PATHS ${CMAKE_CURRENT_SOURCE_DIR}/onnx/include /usr/include /usr/local/include)
find_library(ONNX_LIB onnxruntime PATHS ${CMAKE_CURRENT_SOURCE_DIR}/onnx/lib /usr/lib /usr/local/lib /usr/lib/elanfp)

if(NOT ONNX_INCLUDE_DIR OR NOT ONNX_LIB)
    message(FATAL_ERROR "ONNX Runtime not found. Place headers in ./onnx/include and libs in ./onnx/lib or install system-wide.")
endif()

include_directories(src ${LIBUSB_INCLUDE_DIRS} ${ONNX_INCLUDE_DIR})

# Shared core logic
add_library(elan_core OBJECT
    src/elan_drv.c
    src/image.c
    src/matcher.c
)
set_target_properties(elan_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(COMMON_LIBS ${LIBUSB_LIBRARIES} ${ONNX_LIB} ${MATH_LIB})

# Enroller executable
add_executable(elanfp-enroll src/enroll.c $<TARGET_OBJECTS:elan_core>)
target_link_libraries(elanfp-enroll PRIVATE ${COMMON_LIBS})
set_target_properties(elanfp-enroll PROPERTIES BUILD_RPATH "\$ORIGIN/../lib/elanfp;/usr/lib/elanfp")

# Verification executable
add_executable(elanfp-verify src/verify.c $<TARGET_OBJECTS:elan_core>)
target_link_libraries(elanfp-verify PRIVATE ${COMMON_LIBS})
set_target_properties(elanfp-verify PROPERTIES BUILD_RPATH "\$ORIGIN/../lib/elanfp;/usr/lib/elanfp")

# PAM module (.so)
add_library(pam_elanfp SHARED src/pam_elanfp.c $<TARGET_OBJECTS:elan_core>)
target_link_libraries(pam_elanfp PRIVATE ${COMMON_LIBS} ${PAM_LIB})
set_target_properties(pam_elanfp PROPERTIES
    PREFIX ""
    OUTPUT_NAME "pam_elanfp"
    BUILD_RPATH "/usr/lib/elanfp"
    INSTALL_RPATH "/usr/lib/elanfp"
)

# Install rules
include(GNUInstallDirs)
install(TARGETS elanfp-enroll elanfp-verify RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS pam_elanfp LIBRARY DESTINATION /usr/lib/security)