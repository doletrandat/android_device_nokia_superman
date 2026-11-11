import unittest
from ocv_estimate import estimate


class EstimateTest(unittest.TestCase):
    table = [[0, 23, 45], [100, 4300, 4320], [50, 3800, 3820], [0, 3100, 3100]]

    def test_anchors_temperature_and_bounds(self):
        self.assertEqual(estimate(self.table, 3800000, 230, 0), 50)
        self.assertEqual(estimate(self.table, 3810000, 340, -20000), 50)
        self.assertEqual(estimate(self.table, 4500000, 450, 20000), 100)
        self.assertEqual(estimate(self.table, 2500000, 230, 0), 0)
        last = -1
        for uv in range(2500000, 4500001, 100):
            value = estimate(self.table, uv, 340, 1271)
            self.assertTrue(last <= value <= 100)
            last = value

    def test_invalid_measurements_and_original_column(self):
        for args in [(3700000, 229, 0), (3700000, 451, 0), (2499999, 230, 0),
                     (4500001, 230, 0), (3700000, 230, -20001), (3700000, 230, 20001),
                     (float("nan"), 230, 0), (3700000, True, 0)]:
            with self.assertRaises(ValueError):
                estimate(self.table, *args)
        # The recovered 70C column contains 5%=3183mV followed by 4%=3622mV.
        bad = [[0, 23, 45, 70], [100, 4300, 4320, 4311], [5, 3675, 3659, 3183],
               [4, 3644, 3622, 3622], [0, 3100, 3100, 3100]]
        before = [row[:] for row in bad]
        for temperature in [451, 700]:
            with self.assertRaises(ValueError):
                estimate(bad, 3700000, temperature, 0)
        self.assertGreater(estimate(bad, 3700000, 340, 0), 0)
        self.assertEqual(bad, before)
        for table in [[], [[0, 45, 23], [100, 4300, 4320], [0, 3100, 3100]],
                      [[0, 23, 45], [100, 4300], [0, 3100, 3100]],
                      [[0, 23, 45], [100, 4300, 4320], [100, 3100, 3100]]]:
            with self.assertRaises(ValueError):
                estimate(table, 3700000, 340, 0)


if __name__ == "__main__":
    unittest.main()
