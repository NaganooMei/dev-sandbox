cmake_minimum_required(VERSION 3.16)
project(ascendgdrbw LANGUAGES C CXX)

set(ASCEND_ROOT "/usr/local/Ascend/cann-9.0.0/aarch64-linux" CACHE PATH "Path to Ascend root directory")
set(WORKSPACE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(HCOMM_ROOT ${WORKSPACE_ROOT}/hcomm)
set(HCOMM_SOURCE_ROOT ${WORKSPACE_ROOT}/hcomm/src)

set(ASCEND_ROOT_CANDIDATES
    ${ASCEND_ROOT}
    ${ASCEND_ROOT}/aarch64-linux
)

find_path(ASCEND_RUNTIME_API_PATH acl/acl.h
    PATHS ${ASCEND_ROOT_CANDIDATES}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
)
find_library(ASCEND_RUNTIME_LIB ascendcl
    PATHS ${ASCEND_ROOT_CANDIDATES}
    PATH_SUFFIXES lib64
    NO_DEFAULT_PATH
)
find_path(HCCL_INCLUDE_PATH hccl/hccl_comm.h
    PATHS ${ASCEND_ROOT_CANDIDATES}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
)
find_path(HCCL_PKG_INCLUDE_PATH hccl/base.h
    PATHS
        ${ASCEND_ROOT}
        ${ASCEND_ROOT}/aarch64-linux
        ${ASCEND_ROOT}/..
        ${HCOMM_ROOT}
    PATH_SUFFIXES
        include
        pkg_inc
    NO_DEFAULT_PATH
)
find_library(HCCL_LIB hccl
    PATHS ${ASCEND_ROOT_CANDIDATES}
    PATH_SUFFIXES lib64
    NO_DEFAULT_PATH
)
find_library(HCOMM_LIB hcomm
    PATHS ${ASCEND_ROOT_CANDIDATES}
    PATH_SUFFIXES lib64
    NO_DEFAULT_PATH
)
find_library(C_SEC_LIB c_sec
    PATHS ${ASCEND_ROOT_CANDIDATES}
    PATH_SUFFIXES lib64
    NO_DEFAULT_PATH
)
find_path(IBVERBS_INCLUDE_PATH infiniband/verbs.h PATHS /usr/include /usr/local/include)
find_library(IBVERBS_LIB ibverbs)
find_package(Threads REQUIRED)
find_package(fmt QUIET)

message(STATUS "ASCEND_ROOT=${ASCEND_ROOT}")
message(STATUS "ASCEND_RUNTIME_API_PATH=${ASCEND_RUNTIME_API_PATH}")
message(STATUS "ASCEND_RUNTIME_LIB=${ASCEND_RUNTIME_LIB}")
message(STATUS "HCCL_INCLUDE_PATH=${HCCL_INCLUDE_PATH}")
message(STATUS "HCCL_PKG_INCLUDE_PATH=${HCCL_PKG_INCLUDE_PATH}")
message(STATUS "HCCL_LIB=${HCCL_LIB}")
message(STATUS "HCOMM_LIB=${HCOMM_LIB}")
message(STATUS "C_SEC_LIB=${C_SEC_LIB}")

set(ASCENDGDRBW_COMMON_INCLUDE_DIRS
    ${ASCEND_RUNTIME_API_PATH}
    ${HCCL_INCLUDE_PATH}
    ${HCCL_PKG_INCLUDE_PATH}
    ${HCCL_PKG_INCLUDE_PATH}/hccl
    ${IBVERBS_INCLUDE_PATH}
    ${HCOMM_SOURCE_ROOT}/pub_inc
    ${HCOMM_SOURCE_ROOT}/pub_inc/inner
    ${HCOMM_SOURCE_ROOT}/platform/inc
    ${HCOMM_SOURCE_ROOT}/platform/inc/adapter
    ${HCOMM_SOURCE_ROOT}/framework/op_base/src
    ${HCOMM_SOURCE_ROOT}/framework/common/src/config
)

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND HCCL_INCLUDE_PATH AND HCCL_PKG_INCLUDE_PATH AND HCCL_LIB AND HCOMM_LIB AND C_SEC_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    if(TARGET fmt::fmt)
        add_executable(ascendgdrbw
            ascendgdrbw.cc
            rdma_channel.cc
        )
        target_include_directories(ascendgdrbw PRIVATE ${ASCENDGDRBW_COMMON_INCLUDE_DIRS})
        target_link_libraries(ascendgdrbw PRIVATE
            fmt::fmt
            ${ASCEND_RUNTIME_LIB}
            ${HCCL_LIB}
            ${HCOMM_LIB}
            ${C_SEC_LIB}
            ${IBVERBS_LIB}
            Threads::Threads
        )
    else()
        message(WARNING "fmt was not found, skip target ascendgdrbw; ascendgdrbw_device0_self_h2d_min is still available")
    endif()

    add_executable(ascendgdrbw_device0_self_h2d_min
        device0_self_h2d_min.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw_device0_self_h2d_min PRIVATE ${ASCENDGDRBW_COMMON_INCLUDE_DIRS})
    target_link_libraries(ascendgdrbw_device0_self_h2d_min PRIVATE
        ${ASCEND_RUNTIME_LIB}
        ${HCCL_LIB}
        ${HCOMM_LIB}
        ${C_SEC_LIB}
        ${IBVERBS_LIB}
        Threads::Threads
    )
endif()

