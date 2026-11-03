// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "TemperatureSource.h"
#include <android/hardware/thermal/2.0/IThermal.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace superman {
namespace H = android::hardware;
namespace T = H::thermal::V2_0;
namespace V1 = H::thermal::V1_0;
using android::sp;
using H::Return;

class Thermal : public T::IThermal {
public:
    explicit Thermal(const std::string& root = "/sys/class/thermal");
    ~Thermal() override;
    bool ready() const { return source_.ready(); }
    Return<void> getTemperatures(getTemperatures_cb cb) override;
    Return<void> getCpuUsages(getCpuUsages_cb cb) override;
    Return<void> getCoolingDevices(getCoolingDevices_cb cb) override;
    Return<void> getCurrentTemperatures(bool filter, T::TemperatureType type,
                                       getCurrentTemperatures_cb cb) override;
    Return<void> getTemperatureThresholds(bool filter, T::TemperatureType type,
                                         getTemperatureThresholds_cb cb) override;
    Return<void> getCurrentCoolingDevices(bool filter, T::CoolingType type,
                                         getCurrentCoolingDevices_cb cb) override;
    Return<void> registerThermalChangedCallback(const sp<T::IThermalChangedCallback>& callback,
        bool filter, T::TemperatureType type, registerThermalChangedCallback_cb cb) override;
    Return<void> unregisterThermalChangedCallback(const sp<T::IThermalChangedCallback>& callback,
        unregisterThermalChangedCallback_cb cb) override;
private:
    struct Subscription {
        sp<T::IThermalChangedCallback> callback;
        bool filter, active = true;
        T::TemperatureType type;
        std::recursive_mutex lock;
        std::vector<int> previous;
    };
    TemperatureSource source_;
    std::mutex sourceLock_, callbacksLock_, stopLock_;
    std::condition_variable stopChanged_;
    bool stop_ = false;
    std::vector<std::shared_ptr<Subscription>> callbacks_;
    std::thread worker_;
    void watch();
};
}  // namespace superman
