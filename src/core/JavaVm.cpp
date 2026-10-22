#include "JavaVm.hpp"

#include <atomic>

namespace bedrocktools::core {
namespace {
std::atomic<JavaVM*> gJavaVm{nullptr};
}

void setJavaVm(JavaVM* vm) noexcept {
    gJavaVm.store(vm, std::memory_order_release);
}

JavaVM* javaVm() noexcept {
    return gJavaVm.load(std::memory_order_acquire);
}

} // namespace bedrocktools::core
