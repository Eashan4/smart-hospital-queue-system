"""
app.py
Flask backend for AIoT Smart Hospital Queue & Crowd Monitoring System.
STRICT: No fake data. Device timeout enforced. ML readiness checked.
"""

from flask import Flask, request, jsonify, render_template
from datetime import datetime, timezone
from typing import Optional
from config import HOST, PORT, DEBUG, SECRET_KEY, DEVICE_TIMEOUT, MIN_DATA_THRESHOLD
from data_handler import append_sensor_data, get_latest_entry, get_row_count
import ml_engine

app = Flask(__name__)
app.secret_key = SECRET_KEY

# --- Global device state ---
_last_sensor_time = None  # type: Optional[datetime]
_last_sensor_payload = None  # type: Optional[dict]

# --- Train on startup (may be not_ready if data < threshold) ---
ml_engine.train()


def _device_is_online() -> bool:
    """Check if last sensor ping is within DEVICE_TIMEOUT seconds."""
    if _last_sensor_time is None:
        return False
    now = datetime.now(timezone.utc)
    # Make _last_sensor_time timezone-aware if naive
    lst = _last_sensor_time
    if lst.tzinfo is None:
        lst = lst.replace(tzinfo=timezone.utc)
    elapsed = (now - lst).total_seconds()
    return elapsed <= DEVICE_TIMEOUT


# ─────────────────────────────────────────────
# ROUTES
# ─────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/sensor", methods=["POST"])
def sensor():
    """
    ESP8266 posts real sensor data here.
    Expected JSON: { rfid_tag, people_count, queue_length, wait_time }
    """
    global _last_sensor_time, _last_sensor_payload

    data = request.get_json(silent=True)
    if data is None:
        return jsonify({"error": "Invalid JSON body"}), 400

    required = ["rfid_tag", "people_count", "queue_length", "wait_time"]
    missing = [k for k in required if k not in data]
    if missing:
        return jsonify({"error": f"Missing fields: {missing}"}), 400

    # Type validation
    try:
        people_count = int(data["people_count"])
        queue_length = int(data["queue_length"])
        wait_time = float(data["wait_time"])
        rfid_tag = str(data["rfid_tag"]).strip()
    except (ValueError, TypeError) as e:
        return jsonify({"error": f"Type error: {str(e)}"}), 400

    if not rfid_tag:
        return jsonify({"error": "rfid_tag cannot be empty"}), 400

    # Persist
    record = append_sensor_data(rfid_tag, people_count, queue_length, wait_time)
    _last_sensor_time = datetime.now(timezone.utc)
    _last_sensor_payload = record

    # Retrain if new data crosses threshold
    row_count = get_row_count()
    if row_count >= MIN_DATA_THRESHOLD:
        ml_engine.train()

    return jsonify({"status": "ok", "record": record}), 201


@app.route("/status", methods=["GET"])
def status():
    """
    Returns full system status to frontend.
    STRICT: device_status, model_status, real data or error — never fake values.
    """
    device_online = _device_is_online()
    model_status = ml_engine.get_model_status()

    response = {
        "device_status": "online" if device_online else "offline",
        "model_status": model_status,
        "current_patient": None,
        "queue_length": None,
        "people_count": None,
        "predicted_wait_time": None,
        "anomaly": None,
        "error": None,
    }

    if not device_online:
        response["error"] = "No sensor data received within timeout window"
        return jsonify(response), 200

    # Load latest entry
    latest = get_latest_entry()
    if latest is None:
        response["error"] = "No sensor data in database"
        return jsonify(response), 200

    response["current_patient"] = str(latest.get("rfid_tag", "Unknown"))
    response["queue_length"] = int(latest.get("queue_length", 0))
    response["people_count"] = int(latest.get("people_count", 0))

    # ML prediction
    if model_status == "ready":
        pred = ml_engine.predict(response["people_count"], response["queue_length"])
        response["predicted_wait_time"] = pred.get("predicted_wait_time")
        response["anomaly"] = pred.get("anomaly")
        if pred.get("error"):
            response["error"] = pred["error"]
    else:
        response["error"] = f"ML model not ready — need ≥ {MIN_DATA_THRESHOLD} data rows"

    return jsonify(response), 200


@app.route("/history", methods=["GET"])
def history():
    """Return last N rows as JSON for chart rendering."""
    from data_handler import load_all_data
    df = load_all_data()
    if df.empty:
        return jsonify({"rows": [], "count": 0}), 200

    limit = min(int(request.args.get("limit", 50)), 200)
    df_sorted = df.sort_values("timestamp").tail(limit)
    rows = df_sorted.astype(str).to_dict(orient="records")
    return jsonify({"rows": rows, "count": len(rows)}), 200


@app.route("/train", methods=["POST"])
def retrain():
    """Manually trigger retraining."""
    result = ml_engine.train()
    return jsonify(result), 200


if __name__ == "__main__":
    app.run(host=HOST, port=PORT, debug=DEBUG)
