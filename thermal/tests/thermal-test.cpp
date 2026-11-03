// SPDX-License-Identifier: Apache-2.0
#include "../Thermal.h"
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <fstream>

using namespace superman;
class Callback : public T::IThermalChangedCallback {
public:
    std::mutex lock;
    std::condition_variable changed;
    std::vector<T::Temperature> readings;
    bool entered = false, blocked = false;
    Return<void> notifyThrottling(const T::Temperature& t) override {
        std::unique_lock<std::mutex> guard(lock);
        entered = true;
        changed.notify_all();
        changed.wait(guard, [&] { return !blocked; });
        readings.push_back(t);
        changed.notify_all();
        return H::Void();
    }
    void wait(size_t count) {
        std::unique_lock<std::mutex> guard(lock);
        assert(changed.wait_for(guard, std::chrono::seconds(5), [&] { return readings.size() >= count; }));
    }
};
int main(int argc, char** argv) {
    assert(argc == 2);
    const bool probe = std::string(argv[1]) == "--probe";
    sp<Thermal> thermal = new Thermal(probe ? "/sys/class/thermal" : argv[1]);
    assert(thermal->ready());
    bool passed = false;
    thermal->getCurrentTemperatures(false, T::TemperatureType::CPU, [&](auto status, auto values) {
        assert(status.code == V1::ThermalStatusCode::SUCCESS && values.size() == 3);
        for (const auto& value : values) {
            assert(std::isfinite(value.value));
            printf("%s %.3fC severity=%u\n", value.name.c_str(), value.value, unsigned(value.throttlingStatus));
        }
        passed = true;
    }).isOk();
    assert(passed);
    thermal->getCurrentTemperatures(true, T::TemperatureType::GPU, [&](auto status, auto values) {
        assert(status.code == V1::ThermalStatusCode::SUCCESS && values.size() == 0);
    }).isOk();
    thermal->getTemperatureThresholds(true, T::TemperatureType::CPU, [&](auto status, auto values) {
        assert(status.code == V1::ThermalStatusCode::SUCCESS && values.size() == 2);
        for (const auto& value : values) {
            assert(value.hotThrottlingThresholds[3] == 75 && value.hotThrottlingThresholds[6] == 110);
            assert(std::isnan(value.hotThrottlingThresholds[1]) && std::isnan(value.vrThrottlingThreshold));
        }
    }).isOk();
    thermal->getTemperatureThresholds(true, T::TemperatureType::BATTERY, [&](auto status, auto values) {
        assert(status.code == V1::ThermalStatusCode::SUCCESS && values.size() == 1);
        for (size_t i = 0; i < 7; ++i) assert(std::isnan(values[0].hotThrottlingThresholds[i]));
    }).isOk();
    sp<Callback> callback = new Callback;
    auto success = [](auto s) { assert(s.code == V1::ThermalStatusCode::SUCCESS); };
    auto failure = [](auto s) { assert(s.code == V1::ThermalStatusCode::FAILURE && !s.debugMessage.empty()); };
    thermal->registerThermalChangedCallback(nullptr, false, T::TemperatureType::CPU, failure).isOk();
    thermal->registerThermalChangedCallback(callback, true, T::TemperatureType::BATTERY, success).isOk();
    thermal->registerThermalChangedCallback(callback, false, T::TemperatureType::CPU, failure).isOk();
    callback->wait(1);
    assert(callback->readings[0].type == T::TemperatureType::BATTERY);
    thermal->unregisterThermalChangedCallback(callback, success).isOk();
    thermal->unregisterThermalChangedCallback(callback, failure).isOk();
    if (probe) { puts("PASS real-sensor read-only HIDL object/filter/threshold/callback probe; no service registered"); return 0; }

    const std::string path = std::string(argv[1]) + "/thermal_zone2/temp";
    auto put = [&](const char* value) { std::ofstream file(path); file << value; assert(file.good()); };
    put("broken");
    thermal->getCurrentTemperatures(false, T::TemperatureType::CPU, [&](auto s, auto values) {
        assert(s.code == V1::ThermalStatusCode::FAILURE && values.size() == 3);
        assert(std::isnan(values[0].value));
    }).isOk();
    thermal->getCurrentTemperatures(true, T::TemperatureType::BATTERY, [&](auto s, auto values) {
        assert(s.code == V1::ThermalStatusCode::SUCCESS && values.size() == 1);
    }).isOk();
    put("51000");
    thermal->getCurrentCoolingDevices(false, T::CoolingType::CPU, [&](auto s, auto values) {
        assert(s.code == V1::ThermalStatusCode::SUCCESS && values.size() == 1);
        assert(values[0].type == T::CoolingType::CPU && values[0].value == 1);
    }).isOk();
    sp<Callback> transitions = new Callback;
    thermal->registerThermalChangedCallback(transitions, true, T::TemperatureType::CPU, success).isOk();
    transitions->wait(2);
    put("75000");
    transitions->wait(3);
    assert(transitions->readings.back().throttlingStatus == T::ThrottlingSeverity::SEVERE);
    put("broken");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    assert(transitions->readings.size() == 3);
    put("51000");
    transitions->wait(4);
    assert(transitions->readings.back().throttlingStatus == T::ThrottlingSeverity::NONE);
    thermal->unregisterThermalChangedCallback(transitions, success).isOk();

    // Unregister cannot return while a previous callback is still running.
    sp<Callback> slow = new Callback;
    slow->blocked = true;
    thermal->registerThermalChangedCallback(slow, true, T::TemperatureType::CPU, success).isOk();
    {
        std::unique_lock<std::mutex> lock(slow->lock);
        assert(slow->changed.wait_for(lock, std::chrono::seconds(5), [&] { return slow->entered; }));
    }
    std::mutex lock;
    std::condition_variable changed;
    bool done = false;
    std::thread unregister([&] {
        thermal->unregisterThermalChangedCallback(slow, success).isOk();
        std::lock_guard<std::mutex> guard(lock); done = true; changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        assert(!changed.wait_for(guard, std::chrono::milliseconds(100), [&] { return done; }));
    }
    {
        std::lock_guard<std::mutex> guard(slow->lock); slow->blocked = false; slow->changed.notify_all();
    }
    unregister.join();
    assert(done);
    size_t count = slow->readings.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    assert(slow->readings.size() == count);
    puts("PASS real HIDL filters, thresholds, failure isolation, cooling, callback ownership and in-flight unregister drain");
}
