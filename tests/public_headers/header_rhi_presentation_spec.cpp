#include "rhi/PresentationSpec.h"

// A public header must compile on its own without pulling in Vulkan headers.
namespace {

ar::rhi::DisplayPushData dummyDisplayPush(const ar::rhi::DisplayPushParams&) noexcept {
    return ar::rhi::DisplayPushData{{}, 20u};
}

ar::rhi::PresentationSpec makeSpec() {
    ar::rhi::PresentationSpec spec;
    spec.depthFormat = ar::rhi::AttachmentFormat::D32Sfloat;
    spec.hdrFormat = ar::rhi::AttachmentFormat::Rgba16Sfloat;
    spec.buildDisplayPush = &dummyDisplayPush;
    spec.probes.sampleStrideBytes = 32u;
    return spec;
}

ar::rhi::RawProbeCapture makeRawCapture() {
    ar::rhi::RawProbeCapture capture;
    capture.sampleStrideBytes = 32u;
    capture.includesHdr = true;
    return capture;
}

} // namespace
