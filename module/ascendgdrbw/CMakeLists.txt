set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Path to Ascend root directory")
find_path(ASCEND_RUNTIME_API_PATH acl/acl.h ${ASCEND_ROOT}/include)
find_library(ASCEND_RUNTIME_LIB ascendcl ${ASCEND_ROOT}/lib64)
find_path(IBVERBS_INCLUDE_PATH infiniband/verbs.h /usr/include /usr/local/include)
find_library(IBVERBS_LIB ibverbs)

if(ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    add_executable(ascendgdrbw
        ascendgdrbw.cc
        rdma_channel.cc
    )
    target_include_directories(ascendgdrbw PRIVATE ${ASCEND_RUNTIME_API_PATH} ${IBVERBS_INCLUDE_PATH})
    target_link_libraries(ascendgdrbw PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB} ${IBVERBS_LIB})
endif()
