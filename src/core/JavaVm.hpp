#pragma once

#include <jni.h>

namespace bedrocktools::core {

// Preloader supplies the process JavaVM through ModContext during load.
// Cache it for game-tick callbacks that execute outside the lifecycle scope.
void setJavaVm(JavaVM* vm) noexcept;
JavaVM* javaVm() noexcept;

} // namespace bedrocktools::core
