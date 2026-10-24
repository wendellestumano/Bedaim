#include "xray.hpp"
#include "connectedglass.hpp"

XRayModule::XRayModule()
    : Module("XRay",
             "Hides terrain blocks and exposes ore faces through the existing block tessellation hooks.") {}

void XRayModule::onEnable() {
    if (ConnectedGlassSetXRayMode(true)) return;

    bedrocktools::diagnostics::log(
        bedrocktools::diagnostics::Level::Error,
        moduleId,
        "XRAY activation refused: required block-face/occlusion hooks are unavailable for this runtime.");

    // Do not leave a visually enabled toggle when the native path is unavailable.
    setMasterEnabled(false);
}

void XRayModule::onDisable() {
    ConnectedGlassSetXRayMode(false);
}
