if(NOT UNIX OR WIN32)
    return()
endif()

set(ASCEND_ROOT "${ASCEND_ROOT}" CACHE PATH "Path to Ascend root directory")
if(NOT ASCEND_ROOT)
    if(DEFINED ENV{ASCEND_HOME_PATH} AND NOT "$ENV{ASCEND_HOME_PATH}" STREQUAL "")
        set(ASCEND_ROOT "$ENV{ASCEND_HOME_PATH}")
    elseif(DEFINED ENV{ASCEND_TOOLKIT_HOME} AND NOT "$ENV{ASCEND_TOOLKIT_HOME}" STREQUAL "")
        set(ASCEND_ROOT "$ENV{ASCEND_TOOLKIT_HOME}")
    endif()
endif()

set(HIXL_ROOT "${HIXL_ROOT}" CACHE PATH "Path to HIXL source tree")
if(NOT HIXL_ROOT)
    set(HIXL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../hixl")
endif()

set(HIXL_BUILD_DIR "${HIXL_BUILD_DIR}" CACHE PATH "Path to HIXL build directory")
if(NOT HIXL_BUILD_DIR)
    set(HIXL_BUILD_DIR "${HIXL_ROOT}/build")
endif()

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

message(STATUS "HIXL_ROOT=${HIXL_ROOT}")
message(STATUS "HIXL_BUILD_DIR=${HIXL_BUILD_DIR}")
message(STATUS "HIXL_INCLUDE_PATH=${HIXL_INCLUDE_PATH}")
message(STATUS "HIXL_LIB=${HIXL_LIB}")
message(STATUS "ASCEND_RUNTIME_API_PATH=${ASCEND_RUNTIME_API_PATH}")
message(STATUS "ASCEND_RUNTIME_LIB=${ASCEND_RUNTIME_LIB}")
message(STATUS "METADEF_LIB=${METADEF_LIB}")
message(STATUS "ASCEND_HAL_LIB=${ASCEND_HAL_LIB}")

if(HIXL_INCLUDE_PATH AND HIXL_LIB AND ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND METADEF_LIB)
    add_executable(hixlbw_h2d_async_compare
        h2d_async_compare.cc
    )

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
else()
    message(STATUS "Skipping hixlbw_h2d_async_compare because HIXL/Ascend dependencies were not found.")
endif()
