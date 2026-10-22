#pragma once

#include <jni.h>

namespace bedrocktools::core {

// The Preloader supplies the process JavaVM through ModContext during mod load.
// Cache it because game-tick callbacks run outside the mod lifecycle scope.
void setJavaVm(JavaVM* vm) noexcept;
JavaVM* javaVm() noexcept;

} // namespace bedrocktools::core
