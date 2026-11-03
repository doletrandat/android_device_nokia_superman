// SPDX-License-Identifier: Apache-2.0
#include "../TemperatureSource.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

static void put(const std::string& path, const std::string& text) {
    std::ofstream file(path); file << text; assert(file.good());
}
int main(int argc, char** argv) {
    assert(argc == 2);
    std::string root = argv[1];
    mkdir(root.c_str(), 0700);
    // Deliberately shuffled zone indices and trip indices.
    for (int i = 0; i < 3; ++i) {
        auto zone = root + "/thermal_zone" + std::to_string(i);
        mkdir(zone.c_str(), 0700);
        put(zone + "/type", i == 0 ? "smbb-bif\n" : i == 1 ? "cpu1-thermal\n" : "cpu0-thermal\n");
        put(zone + "/temp", i ? "51000\n" : "38400\n");
        if (!i) continue;
        put(zone + "/trip_point_4_type", "critical\n");
        put(zone + "/trip_point_4_temp", "110000\n");
        put(zone + "/trip_point_4_hyst", "2000\n");
        put(zone + "/trip_point_7_type", "passive\n");
        put(zone + "/trip_point_7_temp", "75000\n");
        put(zone + "/trip_point_7_hyst", "2000\n");
    }
    mkdir((root + "/cooling_device0").c_str(), 0700);
    put(root + "/cooling_device0/type", "cpufreq-cpu0");
    put(root + "/cooling_device0/max_state", "3");
    put(root + "/cooling_device0/cur_state", "1");
    superman::TemperatureSource source(root);
    assert(source.ready());
    std::vector<superman::Reading> r;
    assert(source.read(&r) && r.size() == 3);
    assert(r[0].name == "cpu0-thermal" && r[1].name == "cpu1-thermal" && r[2].type == 2);
    assert(r[0].value == 51 && fabs(r[2].value - 38.4f) < .001f);
    assert(r[0].passive == 75 && r[0].critical == 110 && std::isnan(r[2].critical));
    std::vector<superman::CoolingReading> cooling;
    assert(source.readCooling(&cooling) && cooling.size() == 1 && cooling[0].state == 1);
    for (const auto& bad : {"-1", "4", "nan", "1x", "18446744073709551616"}) {
        put(root + "/cooling_device0/cur_state", bad);
        assert(!source.readCooling(&cooling) && cooling.size() == 0);
    }
    put(root + "/cooling_device0/cur_state", "0");
    assert(source.readCooling(&cooling) && cooling[0].state == 0);
    auto path = root + "/thermal_zone2/temp";
    const int values[] = {74999, 75000, 74000, 73000, 72999, 110000, 109000, 108000, 107999, 72000};
    const int states[] = {0, 3, 3, 3, 0, 6, 6, 6, 3, 0};
    for (size_t i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        put(path, std::to_string(values[i]));
        assert(source.read(&r) && r[0].severity == states[i]);
    }
    put(path, "75000"); assert(source.read(&r) && r[0].severity == 3);
    for (const auto& bad : {"", "nan", "inf", "51000oops", "51000 2", "150001", "-40001", "99999999999999999999999"}) {
        put(path, bad);
        assert(!source.read(&r) && r.size() == 3 && std::isnan(r[0].value) && r[0].severity == 3);
    }
    unlink(path.c_str());
    assert(!source.read(&r) && std::isnan(r[0].value));
    put(path, "-5000\n"); assert(source.read(&r) && r[0].value == -5 && r[0].severity == 0);
    // Thresholds remain static even if writable trip files are changed later.
    put(root + "/thermal_zone2/trip_point_7_temp", "70000");
    assert(source.read(&r) && r[0].passive == 75);
    put(root + "/thermal_zone2/trip_point_7_temp", "110000");
    assert(!superman::TemperatureSource(root).ready());
    put(root + "/thermal_zone2/trip_point_7_temp", "75000");
    put(root + "/thermal_zone2/trip_point_7_hyst", "-1");
    assert(!superman::TemperatureSource(root).ready());
    put(root + "/thermal_zone2/trip_point_7_hyst", "2000");
    put(root + "/thermal_zone2/type", "cpu1-thermal");
    assert(!superman::TemperatureSource(root).ready());
    assert(!superman::TemperatureSource(root + "/missing").ready());
    std::cout << "PASS source identity/order, units, real trips, hysteresis, failure/recovery and static thresholds\n";
}
