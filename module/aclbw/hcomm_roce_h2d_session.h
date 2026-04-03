#ifndef ACLBW_HCOMM_ROCE_H2D_SESSION_H
#define ACLBW_HCOMM_ROCE_H2D_SESSION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class HcommRoceH2dSession {
public:
    HcommRoceH2dSession(int32_t deviceId, void* hostBase, void* deviceBase, size_t totalSize);
    ~HcommRoceH2dSession();

    HcommRoceH2dSession(const HcommRoceH2dSession&) = delete;
    HcommRoceH2dSession& operator=(const HcommRoceH2dSession&) = delete;

    bool IsReady() const noexcept;
    const std::string& ErrorMessage() const noexcept;
    void PrintSummary() const;
    void Write(void* src, void* dst, size_t size) const;
    void Fence() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // ACLBW_HCOMM_ROCE_H2D_SESSION_H
