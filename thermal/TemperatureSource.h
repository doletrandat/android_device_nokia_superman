// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <limits>
#include <cstdint>
#include <string>
#include <vector>

namespace superman {
struct Reading {
    std::string name;
    int type;
    float value = std::numeric_limits<float>::quiet_NaN();
    float passive = std::numeric_limits<float>::quiet_NaN();
    float critical = std::numeric_limits<float>::quiet_NaN();
    int severity = 0;
};
struct CoolingReading { std::string name; uint64_t state = 0; };

class TemperatureSource {
public:
    explicit TemperatureSource(const std::string& root = "/sys/class/thermal");
    bool ready() const { return sensors_.size() == 3; }
    bool read(std::vector<Reading>* readings);
    bool readCooling(std::vector<CoolingReading>* readings) const;
private:
    struct Sensor {
        Reading reading;
        std::string path;
        float passiveHyst = 0, criticalHyst = 0;
    };
    std::vector<Sensor> sensors_;
    struct Cooling { std::string path, name; uint64_t maximum = 0; };
    std::vector<Cooling> cooling_;
    bool coolingValid_ = true;
};
}  // namespace superman
