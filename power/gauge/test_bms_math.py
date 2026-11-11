import unittest
from fractions import Fraction

from bms_math import charge_uah, nominal_soc, signed_cc, voltage_uv


class BmsMathTest(unittest.TestCase):
    def test_counter_sign_and_calibrated_conversion(self):
        for value in [0, 1, -1, (1 << 35) - 1, -(1 << 35)]:
            encoded = (value & ((1 << 36) - 1)).to_bytes(5, "little")
            self.assertEqual(signed_cc(encoded), value)
        for raw in range(-10000000, 10000001, 10000):
            exact = Fraction(raw * 542535 * 3291 * 56 * 1000000,
                             100000 * 3185 * 32764 * 3600 * 8859)
            self.assertLess(abs(charge_uah(raw, 51104, 47919, 8859) - exact), 2)
        self.assertEqual(charge_uah(0, 51104, 47919, 8859), 0)
        self.assertEqual(charge_uah(190890554, 51104, 47919, 8859), 57350)
        for args in [(1 << 35, 51104, 47919, 8859), (0, 47919, 51104, 8859),
                     (0, 51104, 47919, 0), (False, 51104, 47919, 8859)]:
            with self.assertRaises(ValueError):
                charge_uah(*args)
        with self.assertRaises(ValueError):
            signed_cc(bytes(4))

    def test_voltage_and_boot_provenance(self):
        self.assertEqual(voltage_uv(39120, 31166, 37529), 4218822)
        for codes in [(0, 31166, 37529), (65535, 31166, 37529), (39120, 37529, 31166)]:
            with self.assertRaises(ValueError):
                voltage_uv(*codes)
        profile = {"data": {"FCC": 2220, "OCV_vs_TEMP_vs_SOC":
                             [[0, 23, 45], [100, 4300, 4320], [0, 3100, 3100]]}}
        sample = {"boot_id": "A", "OCV_raw": 39120, "BMS_CC_signed36": 190890554, "temp_decic": 368}
        calibration = {"boot_id": "B", "VADC_ref625_raw": 31166, "VADC_ref1250_raw": 37529,
                       "gain_raw": 51104, "offset_raw": 47919, "internal_rsense_uohm": 8859}
        with self.assertRaises(ValueError):
            nominal_soc(profile, sample, calibration)
        calibration["boot_id"] = "A"
        result = nominal_soc(profile, sample, calibration)
        self.assertTrue(0 < result["nominal_profile_soc_percent"] < 100)
        self.assertFalse(result["Android_capacity_changed"])
        self.assertFalse(result["boot_OCV_counter_origin_qualified"])


if __name__ == "__main__":
    unittest.main()
