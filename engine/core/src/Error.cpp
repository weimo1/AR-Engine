#include "core/Error.h"

namespace ar::core {
namespace {

bool sameOrigin(const std::optional<ErrorOrigin>& lhs, const std::optional<ErrorOrigin>& rhs)
{
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs.has_value()) {
        return true;
    }
    return lhs->domain == rhs->domain && lhs->code == rhs->code;
}

} // namespace

bool operator==(const Error& lhs, const Error& rhs)
{
    return lhs.category == rhs.category && lhs.stage == rhs.stage && lhs.message == rhs.message &&
           sameOrigin(lhs.origin, rhs.origin);
}

const char* toString(ErrorCategory category)
{
    switch (category) {
    case ErrorCategory::InvalidArguments:
        return "invalid_arguments";
    case ErrorCategory::InvalidState:
        return "invalid_state";
    case ErrorCategory::LoaderUnavailable:
        return "loader_unavailable";
    case ErrorCategory::LoaderSymbolMissing:
        return "loader_symbol_missing";
    case ErrorCategory::ApiVersionUnsupported:
        return "api_version_unsupported";
    case ErrorCategory::ApiVariantUnsupported:
        return "api_variant_unsupported";
    case ErrorCategory::RequestedApiUnsupported:
        return "requested_api_unsupported";
    case ErrorCategory::InitializationFailed:
        return "initialization_failed";
    case ErrorCategory::QueryFailed:
        return "query_failed";
    case ErrorCategory::DeviceNotFound:
        return "device_not_found";
    case ErrorCategory::CleanupFailed:
        return "cleanup_failed";
    case ErrorCategory::OutputFailed:
        return "output_failed";
    case ErrorCategory::InternalError:
        return "internal_error";
    }
    return "internal_error";
}

const char* toString(ErrorStage stage)
{
    switch (stage) {
    case ErrorStage::Validation:
        return "validation";
    case ErrorStage::Loader:
        return "loader";
    case ErrorStage::Instance:
        return "instance";
    case ErrorStage::Enumeration:
        return "enumeration";
    case ErrorStage::Capability:
        return "capability";
    case ErrorStage::Cleanup:
        return "cleanup";
    case ErrorStage::Output:
        return "output";
    case ErrorStage::Summary:
        return "summary";
    }
    return "validation";
}

const char* toString(ResultKind result)
{
    switch (result) {
    case ResultKind::Success:
        return "success";
    case ResultKind::InvalidArguments:
        return "invalid_arguments";
    case ResultKind::InitializationFailed:
        return "initialization_failed";
    case ResultKind::Unsupported:
        return "unsupported";
    case ResultKind::OutputFailed:
        return "output_failed";
    }
    return "internal_error";
}

} // namespace ar::core
