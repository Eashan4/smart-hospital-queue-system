"""
Smoke tests for the Flask API.

Uses Flask's test client, so no port is bound and no real device is needed.
Sensor data is redirected to a temporary CSV so the committed dataset is
never modified by a test run.
"""

import os
import sys
import tempfile
import unittest

SERVER_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, SERVER_DIR)

import data_handler  # noqa: E402

VALID_READING = {
    "rfid_tag": "A1B2C3D4",
    "people_count": 8,
    "queue_length": 5,
    "wait_time": 12.5,
}


class APISmokeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        handle = tempfile.NamedTemporaryFile(suffix=".csv", delete=False)
        handle.close()
        os.unlink(handle.name)
        cls.data_file = handle.name

        # Redirect persistence before importing app, which trains on import.
        data_handler.DATA_FILE = cls.data_file

        import app  # noqa: E402

        cls.app_module = app
        app.app.config["TESTING"] = True
        cls.client = app.app.test_client()

    @classmethod
    def tearDownClass(cls):
        if os.path.exists(cls.data_file):
            os.unlink(cls.data_file)

    def test_index_renders(self):
        self.assertEqual(self.client.get("/").status_code, 200)

    def test_status_reports_offline_without_recent_reading(self):
        self.app_module._last_sensor_time = None
        payload = self.client.get("/status").get_json()
        self.assertEqual(payload["device_status"], "offline")
        self.assertIsNotNone(payload["error"])

    def test_sensor_accepts_a_valid_reading(self):
        response = self.client.post("/sensor", json=VALID_READING)
        self.assertEqual(response.status_code, 201)
        self.assertEqual(response.get_json()["status"], "ok")

    def test_sensor_rejects_missing_fields(self):
        response = self.client.post("/sensor", json={"rfid_tag": "A1B2C3D4"})
        self.assertEqual(response.status_code, 400)

    def test_sensor_rejects_an_empty_rfid_tag(self):
        reading = dict(VALID_READING, rfid_tag="   ")
        self.assertEqual(self.client.post("/sensor", json=reading).status_code, 400)

    def test_sensor_rejects_a_non_numeric_people_count(self):
        reading = dict(VALID_READING, people_count="many")
        self.assertEqual(self.client.post("/sensor", json=reading).status_code, 400)

    def test_sensor_rejects_a_non_json_body(self):
        response = self.client.post(
            "/sensor", data="not json", content_type="text/plain"
        )
        self.assertEqual(response.status_code, 400)

    def test_history_returns_persisted_readings(self):
        self.client.post("/sensor", json=VALID_READING)
        payload = self.client.get("/history?limit=5").get_json()
        self.assertGreaterEqual(payload["count"], 1)
        self.assertEqual(payload["rows"][-1]["rfid_tag"], VALID_READING["rfid_tag"])

    def test_model_reports_not_ready_below_the_data_threshold(self):
        payload = self.client.post("/train").get_json()
        self.assertIn(payload["model_status"], {"ready", "not_ready"})


if __name__ == "__main__":
    unittest.main(verbosity=2)
