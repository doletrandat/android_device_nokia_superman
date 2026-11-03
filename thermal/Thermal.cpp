// SPDX-License-Identifier: Apache-2.0
#include "Thermal.h"
#include <hidl/HidlTransportSupport.h>
#include <algorithm>
#include <cmath>

namespace superman {
static V1::ThermalStatus status(bool ok, const char* error = "Temperature read failed") {
    return {ok ? V1::ThermalStatusCode::SUCCESS : V1::ThermalStatusCode::FAILURE,
            ok ? "" : error};
}
static T::Temperature convert(const Reading& r) {
    return {static_cast<T::TemperatureType>(r.type), r.name, r.value,
            static_cast<T::ThrottlingSeverity>(r.severity)};
}
Thermal::Thermal(const std::string& root) : source_(root), worker_(&Thermal::watch, this) {}
Thermal::~Thermal() {
    { std::lock_guard<std::mutex> lock(stopLock_); stop_ = true; }
    stopChanged_.notify_all();
    worker_.join();
}

Return<void> Thermal::getCurrentTemperatures(bool filter, T::TemperatureType type,
                                            getCurrentTemperatures_cb cb) {
    std::vector<Reading> readings;
    { std::lock_guard<std::mutex> lock(sourceLock_); source_.read(&readings); }
    std::vector<T::Temperature> temperatures;
    bool ok = ready();
    for (const auto& r : readings) {
        if (filter && r.type != static_cast<int>(type)) continue;
        ok &= std::isfinite(r.value);
        temperatures.push_back(convert(r));
    }
    cb(status(ok), temperatures);
    return H::Void();
}

Return<void> Thermal::getTemperatures(getTemperatures_cb cb) {
    std::vector<Reading> readings;
    bool ok;
    { std::lock_guard<std::mutex> lock(sourceLock_); ok = source_.read(&readings); }
    std::vector<V1::Temperature> temperatures;
    for (const auto& r : readings)
        temperatures.push_back({static_cast<V1::TemperatureType>(r.type), r.name, r.value,
                                r.passive, r.critical, NAN});
    cb(status(ok), temperatures);
    return H::Void();
}

Return<void> Thermal::getTemperatureThresholds(bool filter, T::TemperatureType type,
                                              getTemperatureThresholds_cb cb) {
    std::vector<Reading> readings;
    { std::lock_guard<std::mutex> lock(sourceLock_); source_.read(&readings); }
    std::vector<T::TemperatureThreshold> thresholds;
    for (const auto& r : readings) {
        if (filter && r.type != static_cast<int>(type)) continue;
        T::TemperatureThreshold threshold{};
        threshold.type = static_cast<T::TemperatureType>(r.type);
        threshold.name = r.name;
        for (size_t i = 0; i < 7; ++i) {
            threshold.hotThrottlingThresholds[i] = NAN;
            threshold.coldThrottlingThresholds[i] = NAN;
        }
        threshold.hotThrottlingThresholds[3] = r.passive;
        threshold.hotThrottlingThresholds[6] = r.critical;
        threshold.vrThrottlingThreshold = NAN;
        thresholds.push_back(threshold);
    }
    cb(status(ready(), "Required board thermal zones/trips unavailable"), thresholds);
    return H::Void();
}

Return<void> Thermal::getCpuUsages(getCpuUsages_cb cb) {
    cb(status(false, "CPU usage accounting is not implemented"), {});
    return H::Void();
}
Return<void> Thermal::getCoolingDevices(getCoolingDevices_cb cb) {
    cb(status(false, "No fan RPM interface on this board"), {});
    return H::Void();
}
Return<void> Thermal::getCurrentCoolingDevices(bool filter, T::CoolingType type,
                                              getCurrentCoolingDevices_cb cb) {
    std::vector<CoolingReading> readings;
    bool ok;
    { std::lock_guard<std::mutex> lock(sourceLock_); ok = source_.readCooling(&readings); }
    std::vector<T::CoolingDevice> devices;
    if (!filter || type == T::CoolingType::CPU)
        for (const auto& r : readings) devices.push_back({T::CoolingType::CPU, r.name, r.state});
    cb(status(ok, "CPU cooling state read failed"), devices);
    return H::Void();
}

Return<void> Thermal::registerThermalChangedCallback(const sp<T::IThermalChangedCallback>& callback,
    bool filter, T::TemperatureType type, registerThermalChangedCallback_cb cb) {
    bool added = false;
    if (callback) {
        std::lock_guard<std::mutex> lock(callbacksLock_);
        if (std::none_of(callbacks_.begin(), callbacks_.end(), [&](const auto& item) {
                return H::interfacesEqual(item->callback, callback);
            })) {
            auto item = std::make_shared<Subscription>();
            item->callback = callback;
            item->filter = filter;
            item->type = type;
            item->previous.assign(3, -1);
            callbacks_.push_back(item);
            added = true;
        }
    }
    cb(status(added, "Null or already registered callback"));
    return H::Void();
}

Return<void> Thermal::unregisterThermalChangedCallback(const sp<T::IThermalChangedCallback>& callback,
    unregisterThermalChangedCallback_cb cb) {
    std::shared_ptr<Subscription> removed;
    if (callback) {
        std::lock_guard<std::mutex> lock(callbacksLock_);
        auto it = std::find_if(callbacks_.begin(), callbacks_.end(), [&](const auto& item) {
            return H::interfacesEqual(item->callback, callback);
        });
        if (it != callbacks_.end()) { removed = *it; callbacks_.erase(it); }
    }
    if (removed) {
        // Drain an in-flight delivery; recursive locking also permits a local callback to unregister.
        std::lock_guard<std::recursive_mutex> lock(removed->lock);
        removed->active = false;
    }
    cb(status(bool(removed), "Callback was not registered"));
    return H::Void();
}

void Thermal::watch() {
    std::unique_lock<std::mutex> stopLock(stopLock_);
    while (!stop_) {
        stopLock.unlock();
        std::vector<Reading> readings;
        { std::lock_guard<std::mutex> lock(sourceLock_); source_.read(&readings); }
        std::vector<std::shared_ptr<Subscription>> callbacks;
        { std::lock_guard<std::mutex> lock(callbacksLock_); callbacks = callbacks_; }
        for (const auto& item : callbacks) {
            std::lock_guard<std::recursive_mutex> lock(item->lock);
            for (size_t i = 0; i < readings.size() && item->active; ++i) {
                const auto& r = readings[i];
                if (!std::isfinite(r.value) || (item->filter && r.type != static_cast<int>(item->type)) ||
                    item->previous[i] == r.severity) continue;
                if (!item->callback->notifyThrottling(convert(r)).isOk()) {
                    item->active = false;
                    std::lock_guard<std::mutex> lock(callbacksLock_);
                    callbacks_.erase(std::remove(callbacks_.begin(), callbacks_.end(), item), callbacks_.end());
                    break;
                }
                item->previous[i] = r.severity;
            }
        }
        stopLock.lock();
        stopChanged_.wait_for(stopLock, std::chrono::seconds(1), [&] { return stop_; });
    }
}
}  // namespace superman
