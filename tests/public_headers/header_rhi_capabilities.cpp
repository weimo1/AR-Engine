#include "rhi/Capabilities.h"

// A public header must compile on its own; this translation unit uses the declared types.
namespace {

ar::rhi::CapabilityReport makeReport() {
    ar::rhi::CapabilityReport report;
    report.apiVariants.requested = 0;
    report.instanceCreate.status = ar::rhi::InstanceCreateStatus::NotAttempted;
    report.judgments.apiVersionSupported = false;
    report.descriptorHeap = ar::rhi::DescriptorHeapStatus::NotQueried;
    report.capabilitySource = ar::rhi::CapabilitySource{"system_loader", "", false};
    return report;
}

} // namespace
