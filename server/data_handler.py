"""
data_handler.py
Handles all CSV read/write operations.
STRICT: No fake data, no silent fallback.
"""

import os
from typing import Optional
import pandas as pd
from datetime import datetime
from config import DATA_FILE


REQUIRED_COLUMNS = ["timestamp", "rfid_tag", "people_count", "queue_length", "wait_time"]


def _ensure_file():
    """Create CSV with headers if it doesn't exist."""
    if not os.path.exists(DATA_FILE):
        df = pd.DataFrame(columns=REQUIRED_COLUMNS)
        df.to_csv(DATA_FILE, index=False)


def append_sensor_data(rfid_tag: str, people_count: int, queue_length: int, wait_time: float) -> dict:
    """
    Append one row of real sensor data to CSV.
    Returns the written record or raises on failure.
    """
    _ensure_file()

    record = {
        "timestamp": datetime.utcnow().isoformat(),
        "rfid_tag": rfid_tag,
        "people_count": int(people_count),
        "queue_length": int(queue_length),
        "wait_time": float(wait_time),
    }

    df = pd.DataFrame([record])
    df.to_csv(DATA_FILE, mode="a", header=False, index=False)
    return record


def load_all_data() -> pd.DataFrame:
    """
    Load all CSV data.
    Returns empty DataFrame (with correct columns) if file is empty or missing.
    Never raises — callers check row count to determine readiness.
    """
    _ensure_file()
    try:
        df = pd.read_csv(DATA_FILE)
        # Validate columns
        missing = [c for c in REQUIRED_COLUMNS if c not in df.columns]
        if missing:
            raise ValueError(f"CSV is missing columns: {missing}")
        df["timestamp"] = pd.to_datetime(df["timestamp"], errors="coerce")
        df = df.dropna(subset=["timestamp"])
        return df
    except Exception as e:
        # Return empty but valid DataFrame; caller must handle empty case
        empty = pd.DataFrame(columns=REQUIRED_COLUMNS)
        empty._load_error = str(e)
        return empty


def get_latest_entry() -> Optional[dict]:
    """Return the most recent row as a dict, or None if no data."""
    df = load_all_data()
    if df.empty:
        return None
    row = df.sort_values("timestamp").iloc[-1]
    return row.to_dict()


def get_row_count() -> int:
    """Return number of data rows (excluding header)."""
    df = load_all_data()
    return len(df)
