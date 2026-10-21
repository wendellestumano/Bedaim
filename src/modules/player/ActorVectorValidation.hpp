#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace bedaim::actorsnapshot {

struct VectorMetadata {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    std::uintptr_t capacity = 0;
};

struct ValidationResult {
    bool valid;
    const char* reason;
    std::size_t count;
    std::uintptr_t dataSpan;
};

inline ValidationResult validate(const VectorMetadata& vector,
                                 std::size_t stride,
                                 std::size_t maximumElements) noexcept {
    const auto reject = [](const char* reason, std::uintptr_t span) noexcept {
        return ValidationResult{false, reason, 0, span};
    };
    if (stride == 0) return reject("invalid_stride", 0);
    if (vector.begin == 0 && vector.end == 0 && vector.capacity == 0)
        return {true, "empty", 0, 0};
    if (vector.begin == 0) return reject("null_begin", 0);
    if (vector.end == 0) return reject("null_end", 0);
    if (vector.capacity == 0) return reject("null_capacity", 0);
    if (vector.end < vector.begin) return reject("end_before_begin", 0);
    if (vector.capacity < vector.end) return reject("capacity_before_end", 0);

    const auto dataSpan = vector.end - vector.begin;
    const auto capacitySpan = vector.capacity - vector.begin;
    if (dataSpan % stride != 0) return reject("span_not_stride_aligned", dataSpan);
    if (capacitySpan % stride != 0)
        return reject("capacity_not_stride_aligned", dataSpan);

    const auto count = static_cast<std::size_t>(dataSpan / stride);
    const auto capacityCount = static_cast<std::size_t>(capacitySpan / stride);
    if (count > maximumElements) return reject("count_over_limit", dataSpan);

    const auto maxSize = std::numeric_limits<std::size_t>::max();
    const auto maximumCapacity = maximumElements > maxSize / 4
        ? maxSize : maximumElements * 4;
    if (capacityCount > maximumCapacity)
        return reject("capacity_over_limit", dataSpan);

    return {true, count == 0 ? "empty" : "valid", count, dataSpan};
}

} // namespace bedaim::actorsnapshot
