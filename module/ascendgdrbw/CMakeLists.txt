cmake_minimum_required(VERSION 3.16)
project(ascendgdrbw LANGUAGES C CXX)

set(ASCEND_ROOT "/usr/local/Ascend/cann-9.0.0/aarch64-linux" CACHE PATH "Path to Ascend root directory")

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
find_path(IBVERBS_INCLUDE_PATH infiniband/verbs.h PATHS /usr/include /usr/local/include)
find_library(IBVERBS_LIB ibverbs)
find_package(Threads REQUIRED)
find_package(fmt QUIET)

message(STATUS "ASCEND_ROOT=${ASCEND_ROOT}")
message(STATUS "ASCEND_RUNTIME_API_PATH=${ASCEND_RUNTIME_API_PATH}")
message(STATUS "ASCEND_RUNTIME_LIB=${ASCEND_RUNTIME_LIB}")
message(STATUS "IBVERBS_INCLUDE_PATH=${IBVERBS_INCLUDE_PATH}")
message(STATUS "IBVERBS_LIB=${IBVERBS_LIB}")

set(ASCENDGDRBW_COMMON_INCLUDE_DIRS
    ${ASCEND_RUNTIME_API_PATH}
    ${IBVERBS_INCLUDE_PATH}
)

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    add_executable(ascendgdrbw_device0_self_h2d_min
        device0_self_h2d_min.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw_device0_self_h2d_min PRIVATE
        ${ASCENDGDRBW_COMMON_INCLUDE_DIRS}
    )
    target_link_libraries(ascendgdrbw_device0_self_h2d_min PRIVATE
        ${ASCEND_RUNTIME_LIB}
        ${IBVERBS_LIB}
        Threads::Threads
    )

    if(TARGET fmt::fmt)
        add_executable(ascendgdrbw
            ascendgdrbw.cc
            rdma_channel.cc
        )
        target_include_directories(ascendgdrbw PRIVATE
            ${ASCENDGDRBW_COMMON_INCLUDE_DIRS}
        )
        target_link_libraries(ascendgdrbw PRIVATE
            fmt::fmt
            ${ASCEND_RUNTIME_LIB}
            ${IBVERBS_LIB}
            Threads::Threads
        )
    else()
        message(WARNING "fmt was not found, skip target ascendgdrbw")
    endif()
else()
    message(WARNING "Required core dependencies were not found, skip ascendgdrbw targets")
endif()
