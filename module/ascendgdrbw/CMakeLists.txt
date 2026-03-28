set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Path to Ascend root directory")
find_path(ASCEND_RUNTIME_API_PATH acl/acl.h ${ASCEND_ROOT}/include)
find_library(ASCEND_RUNTIME_LIB ascendcl ${ASCEND_ROOT}/lib64)
find_path(HCCL_INCLUDE_PATH hccl/hccl.h ${ASCEND_ROOT}/include)
find_library(HCCL_LIB hccl ${ASCEND_ROOT}/lib64)
find_path(IBVERBS_INCLUDE_PATH infiniband/verbs.h /usr/include /usr/local/include)
find_library(IBVERBS_LIB ibverbs)
find_package(Threads REQUIRED)

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    add_executable(ascendgdrbw
        ascendgdrbw.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw PRIVATE ${ASCEND_RUNTIME_API_PATH} ${IBVERBS_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${IBVERBS_LIB}
                                                  Threads::Threads)

    add_executable(ascendgdrbw_h2d_demo
        h2d_demo.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw_h2d_demo PRIVATE ${ASCEND_RUNTIME_API_PATH} ${IBVERBS_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw_h2d_demo PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${IBVERBS_LIB}
                                                       Threads::Threads)

    add_executable(ascendgdrbw_mem_probe_demo
        mem_probe_demo.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw_mem_probe_demo PRIVATE ${ASCEND_RUNTIME_API_PATH} ${IBVERBS_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw_mem_probe_demo PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${IBVERBS_LIB}
                                                             Threads::Threads)
endif()

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND HCCL_INCLUDE_PATH AND HCCL_LIB)
    add_executable(ascendgdrbw_hccl_mem_reg_demo
        hccl_mem_reg_demo.cc
    )
    target_include_directories(ascendgdrbw_hccl_mem_reg_demo PRIVATE ${ASCEND_RUNTIME_API_PATH} ${HCCL_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw_hccl_mem_reg_demo PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${HCCL_LIB}
                                                                Threads::Threads)

    add_executable(ascendgdrbw_hccl_h2d_put_demo
        hccl_h2d_put_demo.cc
    )
    target_include_directories(ascendgdrbw_hccl_h2d_put_demo PRIVATE ${ASCEND_RUNTIME_API_PATH} ${HCCL_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw_hccl_h2d_put_demo PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${HCCL_LIB}
                                                                Threads::Threads)
endif()
