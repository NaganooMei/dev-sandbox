set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Path to Ascend root directory")

find_path(ASCEND_RUNTIME_API_PATH acl/acl.h ${ASCEND_ROOT}/include)
find_library(ASCEND_RUNTIME_LIB ascendcl ${ASCEND_ROOT}/lib64)
find_library(ASCEND_HCCL_LIB hccl ${ASCEND_ROOT}/lib64)
find_library(ASCEND_HCOMM_LIB hcomm ${ASCEND_ROOT}/lib64)
if (ASCEND_RUNTIME_API_PATH AND ASCEND_RUNTIME_LIB)
    file(GLOB_RECURSE ACLBW_SOURCE_FILES "*.h" "*.cc")
    add_executable(aclbw ${ACLBW_SOURCE_FILES})
    target_include_directories(aclbw PRIVATE ${ASCEND_RUNTIME_API_PATH})
    target_link_libraries(aclbw PRIVATE fmt::fmt ${ASCEND_RUNTIME_LIB})
    if (ASCEND_HCCL_LIB
        AND EXISTS "${ASCEND_RUNTIME_API_PATH}/hccl/hccl_comm.h"
        AND EXISTS "${ASCEND_RUNTIME_API_PATH}/hccl/hccl_res.h"
        AND EXISTS "${ASCEND_RUNTIME_API_PATH}/hccl/hcomm_primitives.h"
        AND EXISTS "${ASCEND_RUNTIME_API_PATH}/hccl.h")
        target_compile_definitions(aclbw PRIVATE ACLBW_ENABLE_HCOMM_EXPERIMENT)
        target_link_libraries(aclbw PRIVATE ${ASCEND_HCCL_LIB})
        if (ASCEND_HCOMM_LIB)
            target_link_libraries(aclbw PRIVATE ${ASCEND_HCOMM_LIB})
        endif()
    endif()
endif()
