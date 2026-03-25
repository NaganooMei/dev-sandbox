set(CUDA_ROOT "/usr/local/cuda/" CACHE PATH "Path to CUDA root directory")
find_path(CUDA_RUNTIME_API_PATH cuda_runtime.h ${CUDA_ROOT}/include)
find_library(CUDA_RUNTIME_LIB cudart ${CUDA_ROOT}/lib64)
find_path(IBVERBS_INCLUDE_PATH infiniband/verbs.h /usr/include /usr/local/include)
find_library(IBVERBS_LIB ibverbs)

if(CUDA_RUNTIME_API_PATH AND CUDA_RUNTIME_LIB AND IBVERBS_INCLUDE_PATH AND IBVERBS_LIB)
    add_executable(gdrbw
        gdrbw.cc
        rdma_channel.cc
    )
    target_include_directories(gdrbw PRIVATE ${CUDA_RUNTIME_API_PATH} ${IBVERBS_INCLUDE_PATH})
    target_link_libraries(gdrbw PRIVATE fmt::fmt ${CUDA_RUNTIME_LIB} ${IBVERBS_LIB})
endif()
