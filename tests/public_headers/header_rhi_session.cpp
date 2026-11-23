#include "rhi/RhiSession.h"

// A public header must compile on its own; this translation unit uses the declared types.
namespace {

ar::rhi::QueryConfig makeConfig() {
    ar::rhi::QueryConfig config;
    config.loader = ar::rhi::SystemLoader{};
    config.requestedApi = ar::rhi::kApiVersion1_4;
    config.device = ar::rhi::FirstEnumerated{};
    return config;
}

ar::rhi::CloseResult makeClosed() {
    ar::rhi::CloseResult result;
    result.closed = true;
    result.resourcesReleased = true;
    return result;
}

template <class Session>
concept HasVerificationMembers = requires(
                                     Session& session,
                                     std::span<const std::byte> bytes,
                                     std::span<const std::uint32_t> words,
                                     std::span<std::uint32_t> ids,
                                     std::span<std::array<std::uint32_t, 4>> commands) {
    session.verifyGpuCull(bytes, bytes, words, ids, commands);
} || requires(Session& session, std::span<const std::uint32_t> words) {
    session.verifyDescriptorHeapRetirement(words);
};

static_assert(!HasVerificationMembers<ar::rhi::RhiSession>);

} // namespace
