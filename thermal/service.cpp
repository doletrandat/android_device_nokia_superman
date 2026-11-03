// SPDX-License-Identifier: Apache-2.0
#include "Thermal.h"
#include <android-base/logging.h>
#include <hidl/HidlTransportSupport.h>

int main() {
    android::hardware::configureRpcThreadpool(4, true);
    android::sp<superman::Thermal> service = new superman::Thermal;
    if (!service->ready()) { LOG(ERROR) << "Missing Superman thermal zones/trips"; return 1; }
    if (service->registerAsService() != android::OK) return 1;
    android::hardware::joinRpcThreadpool();
    return 1;
}
