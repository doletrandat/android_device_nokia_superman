#!/usr/bin/env python3
"""Inspect the stock BV-T5A voltage curve; never publish Android capacity."""
import argparse
import json
import math
from pathlib import Path


def ocv_percent(table, voltage_uv, temperature_decic):
    """Interpolate a measured open-circuit voltage against the stock curve."""
    if not all(type(x) is int for x in (voltage_uv, temperature_decic)):
        raise ValueError("OCV and temperature must be integer microvolts and deci-C")
    if not 2500000 <= voltage_uv <= 4500000:
        raise ValueError("voltage outside battery range")
    if not isinstance(table, list) or len(table) < 3:
        raise ValueError("missing OCV table")
    if any(not isinstance(row, list) for row in table):
        raise ValueError("OCV rows must be lists")
    width = len(table[0])
    if width < 3 or any(len(row) != width for row in table):
        raise ValueError("invalid OCV table dimensions")
    if any(type(v) is not int for row in table for v in row):
        raise ValueError("OCV entries must be integers")
    temperatures = [x * 10 for x in table[0][1:]]
    rows = table[1:]
    if any(a >= b for a, b in zip(temperatures, temperatures[1:])):
        raise ValueError("temperatures must increase")
    if rows[0][0] != 100 or rows[-1][0] != 0 or any(a[0] <= b[0] for a, b in zip(rows, rows[1:])):
        raise ValueError("SOC anchors must descend from 100 to 0")
    if not temperatures[0] <= temperature_decic <= temperatures[-1]:
        raise ValueError("temperature outside stock calibration")
    hi = next((i for i, t in enumerate(temperatures) if t >= temperature_decic), len(temperatures) - 1)
    lo = hi if temperatures[hi] == temperature_decic else hi - 1
    for column in {lo + 1, hi + 1}:
        values = [row[column] for row in rows]
        if any(not 2500 <= v <= 4500 for v in values) or any(a <= b for a, b in zip(values, values[1:])):
            raise ValueError("nonmonotonic or invalid stock OCV column; original data must be preserved")
    fraction = 0 if lo == hi else (temperature_decic - temperatures[lo]) / (temperatures[hi] - temperatures[lo])
    curve = [row[lo + 1] + fraction * (row[hi + 1] - row[lo + 1]) for row in rows]
    mv = voltage_uv / 1000
    if mv >= curve[0]:
        return 100.0
    if mv <= curve[-1]:
        return 0.0
    i = next(i for i in range(len(curve) - 1) if curve[i] >= mv >= curve[i + 1])
    percent = rows[i + 1][0] + (mv - curve[i + 1]) * (rows[i][0] - rows[i + 1][0]) / (curve[i] - curve[i + 1])
    assert math.isfinite(percent) and 0 <= percent <= 100
    return percent


def estimate(table, voltage_uv, temperature_decic, current_ua):
    """Return a resting-voltage estimate, not a calibrated state of charge."""
    if type(current_ua) is not int or abs(current_ua) > 20000:
        raise ValueError("current above 20mA rest limit or invalid microamps")
    return ocv_percent(table, voltage_uv, temperature_decic)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", type=Path, help="privately recovered DECODED-PROFILE.json")
    parser.add_argument("voltage_uv", type=int)
    parser.add_argument("temperature_decic", type=int)
    parser.add_argument("current_ua", type=int)
    args = parser.parse_args()
    profile = json.loads(args.profile.read_text())
    if profile["source_sha256"] != "7b7622decd9ea01da82bfea6fd5c83ce74b25c9b507f06f191196f3c7e7d7ae9":
        parser.error("profile provenance does not match recovered BV-T5A provision")
    try:
        percent = estimate(profile["data"]["OCV_vs_TEMP_vs_SOC"], args.voltage_uv, args.temperature_decic, args.current_ua)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps({"voltage_curve_percent_estimate": round(percent, 2), "calibrated_SOC": False,
                      "rest_duration_verified": False, "installed_pack_identity_verified": False,
                      "source_sha256": profile["source_sha256"], "Android_capacity_changed": False}))


if __name__ == "__main__":
    main()
