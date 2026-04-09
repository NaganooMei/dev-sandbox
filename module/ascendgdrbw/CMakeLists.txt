set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Path to Ascend root directory")
set(WORKSPACE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(HCOMM_SOURCE_ROOT ${WORKSPACE_ROOT}/hcomm/src)
find_path(ASCEND_RUNTIME_API_PATH acl/acl.h PATHS ${ASCEND_ROOT}/include NO_DEFAULT_PATH)
find_library(ASCEND_RUNTIME_LIB ascendcl PATHS ${ASCEND_ROOT}/lib64 NO_DEFAULT_PATH)
find_path(HCCL_INCLUDE_PATH hccl/hccl_comm.h PATHS ${ASCEND_ROOT}/include NO_DEFAULT_PATH)
find_library(HCCL_LIB hccl PATHS ${ASCEND_ROOT}/lib64 NO_DEFAULT_PATH)
find_library(HCOMM_LIB hcomm PATHS ${ASCEND_ROOT}/lib64 NO_DEFAULT_PATH)
find_library(C_SEC_LIB c_sec PATHS ${ASCEND_ROOT}/lib64 NO_DEFAULT_PATH)
find_path(IBVERBS_INCLUDE_PATH infiniband/verbs.h PATHS /usr/include /usr/local/include)
find_library(IBVERBS_LIB ibverbs)
find_package(Threads REQUIRED)

message(STATUS "ASCEND_ROOT=${ASCEND_ROOT}")
message(STATUS "ASCEND_RUNTIME_API_PATH=${ASCEND_RUNTIME_API_PATH}")
message(STATUS "ASCEND_RUNTIME_LIB=${ASCEND_RUNTIME_LIB}")
message(STATUS "HCCL_INCLUDE_PATH=${HCCL_INCLUDE_PATH}")
message(STATUS "HCCL_LIB=${HCCL_LIB}")
message(STATUS "HCOMM_LIB=${HCOMM_LIB}")
message(STATUS "C_SEC_LIB=${C_SEC_LIB}")

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND HCCL_INCLUDE_PATH AND HCCL_LIB AND HCOMM_LIB AND C_SEC_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    add_executable(ascendgdrbw
        ascendgdrbw.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw PRIVATE
        ${ASCEND_RUNTIME_API_PATH}
        ${HCCL_INCLUDE_PATH}
        ${IBVERBS_INCLUDE_PATH}
        ${HCOMM_SOURCE_ROOT}/pub_inc
        ${HCOMM_SOURCE_ROOT}/pub_inc/inner
        ${HCOMM_SOURCE_ROOT}/platform/inc
        ${HCOMM_SOURCE_ROOT}/platform/inc/adapter
        ${HCOMM_SOURCE_ROOT}/framework/op_base/src
        ${HCOMM_SOURCE_ROOT}/framework/common/src/config
    )
    target_link_libraries(ascendgdrbw PRIVATE
        fmt::fmt
        ${ASCEND_RUNTIME_LIB}
        ${HCCL_LIB}
        ${HCOMM_LIB}
        ${C_SEC_LIB}
        ${IBVERBS_LIB}
        Threads::Threads
    )
endif()

