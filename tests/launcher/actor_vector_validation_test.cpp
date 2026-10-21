#include "modules/player/ActorVectorValidation.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {
int failures = 0;
int checks = 0;

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

using bedaim::actorsnapshot::VectorMetadata;
using bedaim::actorsnapshot::validate;

void expectInvalid(VectorMetadata metadata, std::size_t stride,
                   std::size_t maximum, const char* expectedReason,
                   std::string_view label) {
    const auto result = validate(metadata, stride, maximum);
    check(!result.valid, label);
    check(std::string_view(result.reason) == expectedReason,
          std::string(label) + " reports the expected reason");
}
}

int main() {
    constexpr std::size_t stride = 16;
    constexpr std::size_t maximum = 4;

    const auto allNull = validate({0, 0, 0}, stride, maximum);
    check(allNull.valid && allNull.count == 0 &&
          std::string_view(allNull.reason) == "empty",
          "all-null native snapshot is treated as empty, not dereferenced");

    const auto valid = validate({0x1000, 0x1020, 0x1040}, stride, maximum);
    check(valid.valid && valid.count == 2 && valid.dataSpan == 32,
          "valid snapshot reports its bounded element count");

    const auto reservedEmpty = validate({0x1000, 0x1000, 0x1040}, stride, maximum);
    check(reservedEmpty.valid && reservedEmpty.count == 0,
          "empty snapshot with valid reserved capacity is accepted");

    expectInvalid({0, 0x1010, 0x1020}, stride, maximum,
                  "null_begin", "null begin is rejected");
    expectInvalid({0x1000, 0, 0x1020}, stride, maximum,
                  "null_end", "null end is rejected");
    expectInvalid({0x1000, 0x1010, 0}, stride, maximum,
                  "null_capacity", "null capacity is rejected");
    expectInvalid({0x1020, 0x1010, 0x1030}, stride, maximum,
                  "end_before_begin", "reversed data range is rejected");
    expectInvalid({0x1000, 0x1020, 0x1010}, stride, maximum,
                  "capacity_before_end", "capacity before end is rejected");
    expectInvalid({0x1000, 0x1011, 0x1020}, stride, maximum,
                  "span_not_stride_aligned", "misaligned data span is rejected");
    expectInvalid({0x1000, 0x1010, 0x1028}, stride, maximum,
                  "capacity_not_stride_aligned", "misaligned capacity span is rejected");
    expectInvalid({0x1000, 0x1050, 0x1050}, stride, maximum,
                  "count_over_limit", "too many elements are rejected");
    expectInvalid({0x1000, 0x1000, 0x1110}, stride, maximum,
                  "capacity_over_limit", "oversized capacity is rejected even for an empty range");
    expectInvalid({0x1000, 0x1010, 0x1020}, 0, maximum,
                  "invalid_stride", "zero stride is rejected");

    std::cout << "Actor vector validation: " << (checks - failures)
              << "/" << checks << " checks passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
