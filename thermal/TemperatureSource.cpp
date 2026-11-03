// SPDX-License-Identifier: Apache-2.0
#include "TemperatureSource.h"
#include <cmath>
#include <fstream>
#include <sstream>

namespace superman {
static bool text(const std::string& path, std::string* out) {
    std::ifstream file(path);
    std::string extra;
    return bool(file >> *out) && out->size() <= 64 && !(file >> extra);
}

static bool temperature(const std::string& path, float* out) {
    std::string value;
    if (!text(path, &value)) return false;
    std::istringstream input(value);
    long milli;
    char extra;
    if (!(input >> milli) || (input >> extra) || milli < -40000 || milli > 150000)
        return false;
    *out = milli / 1000.0f;
    return true;
}
static bool state(const std::string& path, uint64_t* out) {
    std::string value;
    if (!text(path, &value) || value.find_first_not_of("0123456789") != std::string::npos)
        return false;
    std::istringstream input(value);
    return bool(input >> *out);
}

TemperatureSource::TemperatureSource(const std::string& root) {
    // Bind by the exact board sensor identities, never by zone numbers.
    for (const auto& name : {"cpu0-thermal", "cpu1-thermal", "smbb-bif"}) {
        Sensor sensor;
        sensor.reading.name = name;
        sensor.reading.type = name == std::string("smbb-bif") ? 2 : 0;
        for (int i = 0; i < 256; ++i) {
            std::string path = root + "/thermal_zone" + std::to_string(i), type;
            if (!text(path + "/type", &type) || type != name) continue;
            if (!sensor.path.empty()) { sensors_.clear(); return; }
            sensor.path = path;
        }
        if (sensor.path.empty()) { sensors_.clear(); return; }
        if (sensor.reading.type == 0) {
            bool passive = false, critical = false;
            for (int i = 0; i < 32; ++i) {
                std::string path = sensor.path + "/trip_point_" + std::to_string(i), type;
                if (!text(path + "_type", &type)) continue;
                if (type != "passive" && type != "critical") continue;
                float value, hyst;
                if (!temperature(path + "_temp", &value) ||
                    !temperature(path + "_hyst", &hyst) || hyst < 0 || value <= hyst) {
                    sensors_.clear(); return;
                }
                if (type == "passive") {
                    if (passive) { sensors_.clear(); return; }
                    passive = true;
                    sensor.reading.passive = value;
                    sensor.passiveHyst = hyst;
                } else {
                    if (critical) { sensors_.clear(); return; }
                    critical = true;
                    sensor.reading.critical = value;
                    sensor.criticalHyst = hyst;
                }
            }
            if (!passive || !critical || sensor.reading.passive >= sensor.reading.critical) {
                sensors_.clear(); return;
            }
        }
        sensors_.push_back(sensor);
    }
    for (int i = 0; i < 256; ++i) {
        Cooling c;
        c.path = root + "/cooling_device" + std::to_string(i);
        if (!text(c.path + "/type", &c.name) || c.name.compare(0, 8, "cpufreq-")) continue;
        if (!state(c.path + "/max_state", &c.maximum)) coolingValid_ = false;
        cooling_.push_back(c);
    }
}

bool TemperatureSource::readCooling(std::vector<CoolingReading>* readings) const {
    readings->clear();
    if (!ready() || !coolingValid_) return false;
    for (const auto& c : cooling_) {
        CoolingReading r;
        r.name = c.name;
        if (!state(c.path + "/cur_state", &r.state) || r.state > c.maximum) {
            readings->clear(); return false;
        }
        readings->push_back(r);
    }
    return true;
}

bool TemperatureSource::read(std::vector<Reading>* readings) {
    readings->clear();
    bool valid = ready();
    for (auto& sensor : sensors_) {
        auto& r = sensor.reading;
        if (!temperature(sensor.path + "/temp", &r.value)) {
            r.value = std::numeric_limits<float>::quiet_NaN();
            valid = false;  // Never turn a failed read into a cool/zero sample.
        } else if (r.type == 0) {
            if (r.value >= r.critical ||
                (r.severity == 6 && r.value >= r.critical - sensor.criticalHyst)) r.severity = 6;
            else if (r.value >= r.passive ||
                (r.severity >= 3 && r.value >= r.passive - sensor.passiveHyst)) r.severity = 3;
            else r.severity = 0;
        }
        // No qualified battery shutdown/skin thresholds exist. Charging policy stays in SMBB.
        readings->push_back(r);
    }
    return valid;
}
}  // namespace superman
