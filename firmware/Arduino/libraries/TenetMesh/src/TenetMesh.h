#pragma once

#include <stdint.h>

enum class TenetRole : uint8_t {
    Gateway,
    TftNormal,
    TftInverse,
    Relay,
    IrGate,
    IrRemote
};

class TenetFirmware {
public:
    struct Impl;

    TenetFirmware(TenetRole role, uint8_t nodeId);
    ~TenetFirmware();

    TenetFirmware(const TenetFirmware &) = delete;
    TenetFirmware &operator=(const TenetFirmware &) = delete;

    void begin();
    void update();

private:
    Impl *impl_;
};