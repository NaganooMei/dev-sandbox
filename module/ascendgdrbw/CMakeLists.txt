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
find_path(ASCEND_BASE_INCLUDE_PATH securec.h
    PATHS
        ${ASCEND_ROOT}
        ${ASCEND_ROOT}/..
        ${ASCEND_ROOT_CANDIDATES}
    PATH_SUFFIXES
        include
        include/base
        pkg_inc
        pkg_inc/base
    NO_DEFAULT_PATH
)
find_path(DLOG_INCLUDE_PATH dlog_pub.h
    PATHS
        ${ASCEND_ROOT}
        ${ASCEND_ROOT}/..
        ${ASCEND_ROOT_CANDIDATES}
        ${HCOMM_ROOT}
    PATH_SUFFIXES
        include
        include/base
        include/log
        pkg_inc
        pkg_inc/base
        pkg_inc/log
        src/platform/hccp/external_depends/inc/log
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
find_library(HCCL_PLF_LIB hccl_plf
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
message(STATUS "ASCEND_BASE_INCLUDE_PATH=${ASCEND_BASE_INCLUDE_PATH}")
message(STATUS "DLOG_INCLUDE_PATH=${DLOG_INCLUDE_PATH}")
message(STATUS "HCCL_LIB=${HCCL_LIB}")
message(STATUS "HCOMM_LIB=${HCOMM_LIB}")
message(STATUS "HCCL_PLF_LIB=${HCCL_PLF_LIB}")
message(STATUS "C_SEC_LIB=${C_SEC_LIB}")

if(NOT ASCEND_BASE_INCLUDE_PATH)
    message(WARNING "securec.h include root was not found under ASCEND_ROOT; the build will rely on system/toolchain include paths for securec.h")
endif()
if(NOT DLOG_INCLUDE_PATH)
    message(WARNING "dlog_pub.h include root was not found under ASCEND_ROOT or hcomm; rdma_channel.cc may still fail to compile")
endif()

set(ASCENDGDRBW_COMMON_INCLUDE_DIRS
    ${ASCEND_RUNTIME_API_PATH}
    ${HCCL_INCLUDE_PATH}
    ${HCCL_PKG_INCLUDE_PATH}
    ${HCCL_PKG_INCLUDE_PATH}/hccl
    ${ASCEND_BASE_INCLUDE_PATH}
    ${DLOG_INCLUDE_PATH}
    ${IBVERBS_INCLUDE_PATH}
    ${HCOMM_SOURCE_ROOT}/pub_inc
    ${HCOMM_SOURCE_ROOT}/platform/inc/adapter
)

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND HCCL_INCLUDE_PATH AND HCCL_PKG_INCLUDE_PATH AND HCCL_LIB AND HCOMM_LIB AND HCCL_PLF_LIB AND C_SEC_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    add_executable(ascendgdrbw_ibv_nic_probe_min
        ibv_nic_probe_min.cc
    )
    target_include_directories(ascendgdrbw_ibv_nic_probe_min PRIVATE
        ${IBVERBS_INCLUDE_PATH}
    )
    target_link_libraries(ascendgdrbw_ibv_nic_probe_min PRIVATE
        ${IBVERBS_LIB}
        Threads::Threads
    )

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
            ${HCCL_PLF_LIB}
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
        ${HCCL_PLF_LIB}
        ${C_SEC_LIB}
        ${IBVERBS_LIB}
        Threads::Threads
    )

    add_executable(ascendgdrbw_device_nic_probe_min
        device_nic_probe_min.cc
    )
    target_include_directories(ascendgdrbw_device_nic_probe_min PRIVATE ${ASCENDGDRBW_COMMON_INCLUDE_DIRS})
    target_link_libraries(ascendgdrbw_device_nic_probe_min PRIVATE
        ${ASCEND_RUNTIME_LIB}
        ${HCCL_LIB}
        ${HCOMM_LIB}
        ${HCCL_PLF_LIB}
        ${C_SEC_LIB}
        ${IBVERBS_LIB}
        Threads::Threads
    )
else()
    message(WARNING "Required core dependencies were not found, skip target ascendgdrbw_device0_self_h2d_min")
endif()

