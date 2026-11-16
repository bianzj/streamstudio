"""Native energy-record layout tests (no GPU)."""
import unittest

from thermal_imaging import BAND_POINTS, energy_record_width


class EnergyRecordLayoutTests(unittest.TestCase):
    def test_single_band_is_13_floats(self):
        self.assertEqual(energy_record_width({'configuration': {'sensor': {'bands': '10000'}}}), 13)
        self.assertEqual(energy_record_width({'sensor': {'bands': '10000'}}), 13)

    def test_two_bands_add_two_values(self):
        self.assertEqual(energy_record_width({'configuration': {'sensor': {'bands': '8000,9000'}}}), 15)
        self.assertEqual(energy_record_width({'configuration': {'sensor': {'bands': [8000, 9000]}}}), 15)

    def test_missing_band_defaults_to_the_single_band_layout(self):
        self.assertEqual(energy_record_width({'configuration': {'sensor': {}}}), 13)

    def test_empty_band_list_is_rejected(self):
        with self.assertRaises(ValueError):
            energy_record_width({'configuration': {'sensor': {'bands': ''}}})

    def test_band_points_matches_the_integrator(self):
        self.assertEqual(BAND_POINTS, 17)


if __name__ == '__main__':
    unittest.main()
