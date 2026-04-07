set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Path to Ascend root directory")
find_path(ASCEND_RUNTIME_API_PATH acl/acl.h PATHS ${ASCEND_ROOT}/include NO_DEFAULT_PATH)
find_library(ASCEND_RUNTIME_LIB ascendcl PATHS ${ASCEND_ROOT}/lib64 NO_DEFAULT_PATH)
find_path(HCCL_INCLUDE_PATH hccl/hccl_comm.h PATHS ${ASCEND_ROOT}/include NO_DEFAULT_PATH)
find_library(HCCL_LIB NAMES hccl hcomm PATHS ${ASCEND_ROOT}/lib64 NO_DEFAULT_PATH)
find_path(IBVERBS_INCLUDE_PATH infiniband/verbs.h PATHS /usr/include /usr/local/include)
find_library(IBVERBS_LIB ibverbs)
find_package(Threads REQUIRED)

message(STATUS "ASCEND_ROOT=${ASCEND_ROOT}")
message(STATUS "ASCEND_RUNTIME_API_PATH=${ASCEND_RUNTIME_API_PATH}")
message(STATUS "ASCEND_RUNTIME_LIB=${ASCEND_RUNTIME_LIB}")
message(STATUS "HCCL_INCLUDE_PATH=${HCCL_INCLUDE_PATH}")
message(STATUS "HCCL_LIB=${HCCL_LIB}")

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    add_executable(ascendgdrbw
        ascendgdrbw.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw PRIVATE ${ASCEND_RUNTIME_API_PATH} ${IBVERBS_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${IBVERBS_LIB}
                                                  Threads::Threads)
endif()

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND HCCL_INCLUDE_PATH AND HCCL_LIB)
    add_executable(ascendgdrbw_hccl_h2d_put_demo
        hccl_h2d_put_demo.cc
    )
    target_include_directories(ascendgdrbw_hccl_h2d_put_demo PRIVATE ${ASCEND_RUNTIME_API_PATH} ${HCCL_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw_hccl_h2d_put_demo PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${HCCL_LIB}
                                                                Threads::Threads)

    add_executable(ascendgdrbw_hccl_channel_demo
        hccl_channel_demo.cc
    )
    target_include_directories(ascendgdrbw_hccl_channel_demo PRIVATE ${ASCEND_RUNTIME_API_PATH} ${HCCL_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw_hccl_channel_demo PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${HCCL_LIB}
                                                                Threads::Threads)
else()
    message(WARNING "Skipping ascendgdrbw_hccl demos because HCCL headers or library were not found under ${ASCEND_ROOT}")
endif()
