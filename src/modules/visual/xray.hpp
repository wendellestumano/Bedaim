#pragma once

#include "../Module.hpp"

class XRayModule final : public Module {
public:
    XRayModule();
    void onEnable() override;
    void onDisable() override;
};
