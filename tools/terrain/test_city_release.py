import unittest
from run_city_release import evaluate


class CityReleaseTests(unittest.TestCase):
    def test_partial_tiles_fail_despite_old_success_flag(self):
        report = {"runtimeBuilt": True, "jobs": [{"built": True}, {"built": False}],
                  "release": {"geometryAccepted": True, "viewsReviewed": True}}
        self.assertIn("incomplete-runtime-terrain", evaluate(report, 0))

    def test_absent_evidence_is_not_success(self):
        self.assertEqual(evaluate({}, 0), ["geometry-not-accepted", "incomplete-runtime-terrain", "visual-review-pending"])

    def test_process_failure_cannot_be_hidden_by_report(self):
        report = {"jobs": [{"built": True}], "release": {"geometryAccepted": True, "viewsReviewed": True}}
        self.assertEqual(evaluate(report, 4), ["generator-exit-4"])

    def test_native_rejections_are_preserved(self):
        report = {"jobs": [{"built": True}], "release": {"geometryAccepted": False,
                  "failures": ["historical-building-unresolved"], "viewsReviewed": False}}
        self.assertIn("historical-building-unresolved", evaluate(report, 0))

    def test_render_without_review_is_pending(self):
        report = {"jobs": [{"built": True}], "release": {"geometryAccepted": True, "viewsReviewed": False}}
        self.assertEqual(evaluate(report, 0), ["visual-review-pending"])


if __name__ == "__main__":
    unittest.main()
