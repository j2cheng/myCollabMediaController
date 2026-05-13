#pragma once

#include <cstdint>

namespace DeviceMock {
    inline uint32_t getDeviceId() { return 0xff00; }

    //mk2 app : mcImpl.setDeviceIdProvider([]{ return RealDevice::getId(); });
}

