#ifndef ACLBW_HCOMM_EXPERIMENTAL_API_H
#define ACLBW_HCOMM_EXPERIMENTAL_API_H

#include <arpa/inet.h>
#include <cstdint>

#include <hccl/hccl_comm.h>
#include <hccl/hccl_res.h>
#include <hccl/hccl_types.h>
#include <hccl/hcomm_primitives.h>

extern "C" {

typedef enum {
    ACLBW_COMM_PROTOCOL_RESERVED = -1,
    ACLBW_COMM_PROTOCOL_HCCS = 0,
    ACLBW_COMM_PROTOCOL_TCP = 1,
    ACLBW_COMM_PROTOCOL_ROCE = 2,
    ACLBW_COMM_PROTOCOL_UB_CTP = 3,
    ACLBW_COMM_PROTOCOL_UB_TP = 4,
    ACLBW_COMM_PROTOCOL_PCIE = 5,
    ACLBW_COMM_PROTOCOL_SIO = 6,
} AclbwCommProtocol;

typedef enum {
    ACLBW_COMM_ADDR_TYPE_RESERVED = -1,
    ACLBW_COMM_ADDR_TYPE_IP_V4 = 0,
    ACLBW_COMM_ADDR_TYPE_IP_V6 = 1,
    ACLBW_COMM_ADDR_TYPE_ID = 2,
} AclbwCommAddrType;

typedef struct {
    AclbwCommAddrType type;
    union {
        uint8_t raws[36];
        struct in_addr addr;
        struct in6_addr addr6;
        uint32_t id;
    };
} AclbwCommAddr;

typedef enum {
    ACLBW_END_POINT_LOCATION_RESERVED = -1,
    ACLBW_END_POINT_LOCATION_HOST = 0,
    ACLBW_END_POINT_LOCATION_DEVICE = 1,
} AclbwEndPointLocation;

typedef struct {
    int64_t devId;
    AclbwEndPointLocation location;
} AclbwEndPointLoc;

typedef struct {
    AclbwCommProtocol protocol;
    AclbwCommAddr commAddr;
} AclbwEndPoint;

typedef struct {
    void *addr;
    uint64_t len;
} AclbwHcommBuf;

typedef enum {
    ACLBW_HCCL_MEM_TYPE_DEVICE,
    ACLBW_HCCL_MEM_TYPE_HOST,
    ACLBW_HCCL_MEM_TYPE_NUM,
} AclbwHcclMemType;

typedef struct {
    AclbwHcclMemType type;
    void *addr;
    uint64_t size;
} AclbwHcclMem;

typedef void *AclbwEndPointHandle;

typedef enum {
    ACLBW_COMM_ENGINE_RESERVED = -1,
    ACLBW_COMM_ENGINE_CPU = 0,
    ACLBW_COMM_ENGINE_CPU_TS = 1,
    ACLBW_COMM_ENGINE_AICPU = 2,
    ACLBW_COMM_ENGINE_AICPU_TS = 3,
    ACLBW_COMM_ENGINE_AIV = 4,
    ACLBW_COMM_ENGINE_CCU = 5,
} AclbwCommEngine;

typedef struct {
    AclbwEndPoint remoteEndPoint;
    uint32_t notifyNum;
    union {
        uint8_t raws[128];
        struct {
            uint32_t queueNum;
            uint32_t retryCnt;
            uint32_t retryInterval;
            uint8_t tc;
            uint8_t sl;
        } roceAttr;
    };
} AclbwHcommChannelDesc;

HcclResult HcommEndPointGet(int32_t deviceId, AclbwEndPoint **endPointList, uint32_t *listNum);
HcclResult HcommEndPointCreate(const AclbwEndPoint *endPoint, AclbwEndPointHandle *endPointHandle);
HcclResult HcommEndPointDestroy(AclbwEndPointHandle endPointHandle);
HcclResult HcommMemReg(AclbwEndPointHandle endPointHandle, AclbwHcclMem mem, void **memHandle);
HcclResult HcommMemUnReg(AclbwEndPointHandle endPointHandle, void *memHandle);
HcclResult HcommMemExport(AclbwEndPointHandle endPointHandle, const void *memHandle, void **memDesc,
                          uint32_t *memDescLen);
HcclResult HcommMemImport(AclbwEndPointHandle endPointHandle, const void *memDesc, uint32_t descLen,
                          AclbwHcommBuf *outBuf);
HcclResult HcommMemClose(AclbwEndPointHandle endPointHandle, const AclbwHcommBuf *buf);
HcclResult HcommChannelCreate(AclbwEndPointHandle *endPointHandle, AclbwCommEngine engine,
                              AclbwHcommChannelDesc *channelDescList, uint32_t listNum,
                              const void **memHandleList, uint32_t memHandleListNum,
                              ChannelHandle *channelList);
HcclResult HcommChannelDestroy(const ChannelHandle *channelList, uint32_t listNum);
HcclResult HcommChannelFenceOnThread(ThreadHandle thread, ChannelHandle channel);

}  // extern "C"

#endif  // ACLBW_HCOMM_EXPERIMENTAL_API_H
