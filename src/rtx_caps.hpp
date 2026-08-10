#pragma once
#include <string>

namespace dense {
struct GpuCapabilities {
    std::wstring adapter=L"Unknown GPU";
    unsigned dedicatedMemoryMiB=0;
    bool directX12=false;
    unsigned rayTracingTier=0;
    unsigned meshShaderTier=0;
    unsigned samplerFeedbackTier=0;
};
GpuCapabilities queryGpuCapabilities();
}
