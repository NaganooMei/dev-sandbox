cmake_minimum_required(VERSION 3.16.0)
project(hixlbw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HIXLBW_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(HIXLBW_SOURCE "${HIXLBW_DIR}/h2d_async_compare.cc")

set(ASCEND_ROOT "${ASCEND_ROOT}" CACHE PATH "Path to Ascend root directory")
if(NOT ASCEND_ROOT)
    if(DEFINED ENV{ASCEND_HOME_PATH} AND NOT "$ENV{ASCEND_HOME_PATH}" STREQUAL "")
        set(ASCEND_ROOT "$ENV{ASCEND_HOME_PATH}")
    elseif(DEFINED ENV{ASCEND_TOOLKIT_HOME} AND NOT "$ENV{ASCEND_TOOLKIT_HOME}" STREQUAL "")
        set(ASCEND_ROOT "$ENV{ASCEND_TOOLKIT_HOME}")
    endif()
endif()

set(HIXL_ROOT "${HIXL_ROOT}" CACHE PATH "Path to HIXL source tree")
set(HIXL_BUILD_DIR "${HIXL_BUILD_DIR}" CACHE PATH "Path to HIXL build directory")

find_path(HIXL_INCLUDE_PATH
    NAMES hixl/hixl.h
    PATHS "${HIXL_ROOT}/include"
    NO_DEFAULT_PATH
)

find_library(HIXL_LIB
    NAMES cann_hixl
    PATHS
        "${HIXL_BUILD_DIR}/src/llm_datadist"
        "${HIXL_BUILD_DIR}/build/src/llm_datadist"
        "${HIXL_BUILD_DIR}/src/hixl"
        "${HIXL_ROOT}/build/src/llm_datadist"
        "${HIXL_ROOT}/build/src/hixl"
        "${HIXL_ROOT}/build_out/lib"
        "${HIXL_ROOT}/build_out/lib64"
        "${HIXL_ROOT}/build_out/hixl/lib"
    NO_DEFAULT_PATH
)

find_path(ASCEND_RUNTIME_API_PATH
    NAMES acl/acl.h
    PATHS "${ASCEND_ROOT}/include"
    NO_DEFAULT_PATH
)

find_library(ASCEND_RUNTIME_LIB
    NAMES ascendcl
    PATHS
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ROOT}/runtime/lib64"
    NO_DEFAULT_PATH
)

find_library(METADEF_LIB
    NAMES metadef
    PATHS
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ROOT}/runtime/lib64"
    NO_DEFAULT_PATH
)

find_library(ASCEND_HAL_LIB
    NAMES ascend_hal
    PATHS
        "${ASCEND_ROOT}/lib64/stub"
        "${ASCEND_ROOT}/runtime/lib64/stub"
        "${ASCEND_ROOT}/devlib"
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ROOT}/runtime/lib64"
    NO_DEFAULT_PATH
)

message(STATUS "ASCEND_ROOT=${ASCEND_ROOT}")
message(STATUS "HIXL_ROOT=${HIXL_ROOT}")
message(STATUS "HIXL_BUILD_DIR=${HIXL_BUILD_DIR}")
message(STATUS "HIXL_INCLUDE_PATH=${HIXL_INCLUDE_PATH}")
message(STATUS "HIXL_LIB=${HIXL_LIB}")
message(STATUS "ASCEND_RUNTIME_API_PATH=${ASCEND_RUNTIME_API_PATH}")
message(STATUS "ASCEND_RUNTIME_LIB=${ASCEND_RUNTIME_LIB}")
message(STATUS "METADEF_LIB=${METADEF_LIB}")
message(STATUS "ASCEND_HAL_LIB=${ASCEND_HAL_LIB}")

if(NOT HIXL_INCLUDE_PATH OR NOT HIXL_LIB OR NOT ASCEND_RUNTIME_API_PATH OR NOT ASCEND_RUNTIME_LIB OR NOT METADEF_LIB)
    message(FATAL_ERROR "Missing HIXL/Ascend dependencies for hixlbw_h2d_async_compare")
endif()

add_executable(hixlbw_h2d_async_compare "${HIXLBW_SOURCE}")

target_compile_definitions(hixlbw_h2d_async_compare PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
target_compile_options(hixlbw_h2d_async_compare PRIVATE
    -ftrapv
    -O2
    -fno-common
    -Wfloat-equal
    -Wall
    -Werror
    -Wextra
)
target_include_directories(hixlbw_h2d_async_compare PRIVATE
    "${HIXL_INCLUDE_PATH}"
    "${ASCEND_RUNTIME_API_PATH}"
)
target_link_libraries(hixlbw_h2d_async_compare PRIVATE
    "${HIXL_LIB}"
    "${ASCEND_RUNTIME_LIB}"
    "${METADEF_LIB}"
    pthread
    rt
)

if(ASCEND_HAL_LIB)
    target_link_libraries(hixlbw_h2d_async_compare PRIVATE "${ASCEND_HAL_LIB}")
endif()
