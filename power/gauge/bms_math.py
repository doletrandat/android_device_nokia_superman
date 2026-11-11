#!/usr/bin/env python3
"""Convert coherent PM8226 BMS1 data; diagnostic only, no device writes."""
import argparse
import json
from pathlib import Path

from ocv_estimate import ocv_percent


def trunc_div(numerator, denominator):
    if denominator <= 0:
        raise ValueError("nonpositive conversion denominator")
    return abs(numerator) // denominator * (-1 if numerator < 0 else 1)


def signed_cc(data):
    if not isinstance(data, bytes) or len(data) != 5:
        raise ValueError("BMS1 counter must contain five bytes")
    value = int.from_bytes(data, "little") & ((1 << 36) - 1)
    return value - (1 << 36) if value & (1 << 35) else value


def charge_uah(counter, gain, offset, rsense_uohm):
    """Qualcomm BMS1 conversion; positive counter means consumed charge."""
    if not all(type(x) is int for x in (counter, gain, offset, rsense_uohm)):
        raise ValueError("counter and calibration must be integers")
    if not -(1 << 35) <= counter < (1 << 35):
        raise ValueError("counter outside signed 36-bit range")
    if not 0 <= offset < gain <= 65535 or not 1000 <= gain - offset <= 10000:
        raise ValueError("invalid IADC gain/offset calibration")
    if not 1000 <= rsense_uohm <= 50000:
        raise ValueError("invalid calibrated sense resistance")
    uv = trunc_div(counter * 542535, 100000)
    uv = trunc_div(uv * 3291, gain - offset)
    pvh = trunc_div(uv * 56 * 100000, 32764 * 3600) * 10
    return trunc_div(pvh, rsense_uohm)


def voltage_uv(raw, reference625, reference1250):
    if not all(type(x) is int for x in (raw, reference625, reference1250)):
        raise ValueError("VADC codes must be integers")
    if not 0x6000 < raw < 65535 or not 0x6000 < reference625 < reference1250 < 65535:
        raise ValueError("invalid BMS voltage or VADC reference code")
    if not 1000 <= reference1250 - reference625 <= 20000:
        raise ValueError("invalid VADC reference span")
    value = 3 * (625000 + trunc_div((raw - reference625) * 625000, reference1250 - reference625))
    if not 2500000 <= value <= 4500000:
        raise ValueError("converted battery voltage outside range")
    return value


def nominal_soc(profile, sample, calibration):
    if not sample.get("boot_id") or sample["boot_id"] != calibration.get("boot_id"):
        raise ValueError("sample and calibration must belong to the same boot")
    data = profile["data"]
    fcc = data["FCC"]
    if type(fcc) is not int or not 100 <= fcc <= 10000:
        raise ValueError("invalid nominal profile capacity")
    uv = voltage_uv(sample["OCV_raw"], calibration["VADC_ref625_raw"], calibration["VADC_ref1250_raw"])
    consumed = charge_uah(sample["BMS_CC_signed36"], calibration["gain_raw"],
                          calibration["offset_raw"], calibration["internal_rsense_uohm"])
    percent = ocv_percent(data["OCV_vs_TEMP_vs_SOC"], uv, sample["temp_decic"]) - consumed * 100 / (fcc * 1000)
    # shortcut: boot OCV and counter origin assumed paired; qualify startup before publishing capacity.
    return {"nominal_profile_soc_percent": max(0.0, min(100.0, percent)), "OCV_uV": uv,
            "hardware_counter_consumed_uAh": consumed, "nominal_capacity_uAh": fcc * 1000,
            "boot_OCV_counter_origin_qualified": False, "learned_capacity_qualified": False,
            "Android_capacity_changed": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("profile", "sample", "calibration"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    try:
        result = nominal_soc(*(json.loads(path.read_text()) for path in (args.profile, args.sample, args.calibration)))
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    print(json.dumps(result))
