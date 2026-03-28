#ifndef ASCENDGDRBW_HCCL_ONE_SIDED_SERVICES_H
#define ASCENDGDRBW_HCCL_ONE_SIDED_SERVICES_H

#include <cstdint>

#include <acl/acl.h>
#include <hccl/hccl.h>
#include <hccl/hccl_types.h>

// These declarations mirror the one-sided HCCL service wrappers found in the
// open-source HCCL tree. They are not part of the minimal public headers that
// ship with this repo, so we keep a tiny local shim for experiments.

enum AscendgdrbwHcclMemType {
    ASCENDGDRBW_HCCL_MEM_TYPE_DEVICE = 0,
    ASCENDGDRBW_HCCL_MEM_TYPE_HOST = 1,
};

struct HcclOneSidedMem {
    int type;
    void* addr;
    uint64_t size;
};

constexpr uint32_t kHcclMemDescLength = 511;

struct HcclOneSidedMemDesc {
    char desc[kHcclMemDescLength + 1];
};

struct HcclOneSidedMemDescs {
    HcclOneSidedMemDesc* array;
    uint32_t arrayLength;
};

struct HcclOneSidedOpDesc {
    void* localAddr;
    void* remoteAddr;
    uint64_t count;
    HcclDataType dataType;
};

extern "C" {

HcclResult HcclRegisterMem(HcclComm comm,
                           uint32_t remoteRank,
                           int type,
                           void* addr,
                           uint64_t size,
                           HcclOneSidedMemDesc* desc);
HcclResult HcclDeregisterMem(HcclComm comm, HcclOneSidedMemDesc* desc);
HcclResult HcclExchangeMemDesc(HcclComm comm,
                               uint32_t remoteRank,
                               HcclOneSidedMemDescs* local,
                               int timeout,
                               HcclOneSidedMemDescs* remote,
                               uint32_t* actualNum);
HcclResult HcclEnableMemAccess(HcclComm comm,
                               HcclOneSidedMemDesc* remoteMemDesc,
                               HcclOneSidedMem* remoteMem);
HcclResult HcclDisableMemAccess(HcclComm comm, HcclOneSidedMemDesc* remoteMemDesc);
HcclResult HcclBatchPut(HcclComm comm,
                        uint32_t remoteRank,
                        HcclOneSidedOpDesc* desc,
                        uint32_t descNum,
                        aclrtStream stream);

bool HcommIsSupportHcclRegisterMem(void);
bool HcommIsSupportHcclExchangeMemDesc(void);
bool HcommIsSupportHcclEnableMemAccess(void);
bool HcommIsSupportHcclDisableMemAccess(void);
bool HcommIsSupportHcclBatchPut(void);

}

#endif  // ASCENDGDRBW_HCCL_ONE_SIDED_SERVICES_H
