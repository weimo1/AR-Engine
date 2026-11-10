#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ar::core {

// Fixed failure categories. Modules assign the category; callers compare enum values and
// never parse message text.
enum class ErrorCategory {
    InvalidArguments,
    InvalidState,
    LoaderUnavailable,
    LoaderSymbolMissing,
    ApiVersionUnsupported,
    ApiVariantUnsupported,
    RequestedApiUnsupported,
    InitializationFailed,
    QueryFailed,
    DeviceNotFound,
    CleanupFailed,
    OutputFailed,
    InternalError,
};

// Fixed failure stages. The stage names the operation that failed, not the module that owns
// the error value.
enum class ErrorStage {
    Validation,
    Loader,
    Instance,
    Enumeration,
    Capability,
    Cleanup,
    Output,
    Summary,
};

// Business result of a run. core stores it and never derives product eligibility from it.
enum class ResultKind {
    Success,
    InvalidArguments,
    InitializationFailed,
    Unsupported,
    OutputFailed,
};

// Original low-level result. domain is one of os / vulkan / filesystem / internal; code keeps
// the raw signed integer (rhi preserves the original VkResult).
struct ErrorOrigin {
    std::string domain;
    std::int64_t code = 0;
};

struct Error {
    ErrorCategory category = ErrorCategory::InternalError;
    ErrorStage stage = ErrorStage::Validation;
    std::string message;
    std::optional<ErrorOrigin> origin;
};

bool operator==(const Error& lhs, const Error& rhs);
inline bool operator!=(const Error& lhs, const Error& rhs) {
    return !(lhs == rhs);
}

// Stable names used by the text stream and the JSONL stream.
const char* toString(ErrorCategory category);
const char* toString(ErrorStage stage);
const char* toString(ResultKind result);

} // namespace ar::core
