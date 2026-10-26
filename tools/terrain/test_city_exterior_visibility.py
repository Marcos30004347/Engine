"""Exterior joinery must survive overview filtering."""
import unittest
from render_city_model import OVERVIEW_MATERIALS

class ExteriorVisibilityTests(unittest.TestCase):
    def test_timber_frames_shutters_and_door_leaves_remain_visible(self):
        self.assertTrue({17,45}.issubset(OVERVIEW_MATERIALS))
        self.assertTrue({16,44}.issubset(OVERVIEW_MATERIALS))
        self.assertNotIn(13, OVERVIEW_MATERIALS)  # interior wall skins

if __name__ == '__main__':
    unittest.main()
