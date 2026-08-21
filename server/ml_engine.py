"""
ml_engine.py
Trains and runs RandomForestRegressor + IsolationForest.
STRICT: Returns model_status='not_ready' if dataset < MIN_DATA_THRESHOLD.
No predictions are made on insufficient data.
"""

import numpy as np
import pandas as pd
from typing import Optional
from sklearn.ensemble import RandomForestRegressor, IsolationForest
from sklearn.preprocessing import StandardScaler
from config import MIN_DATA_THRESHOLD
from data_handler import load_all_data

# --- Module-level state ---
_model = None           # type: Optional[RandomForestRegressor]
_anomaly_model = None   # type: Optional[IsolationForest]
_scaler = None          # type: Optional[StandardScaler]
_is_ready = False       # type: bool
_train_error = None     # type: Optional[str]



def _engineer_features(df: pd.DataFrame) -> pd.DataFrame:
    """
    Feature engineering:
    - rolling_avg_people: rolling mean of last 5 people_count entries
    - entry_velocity: diff of people_count (rate of change)
    - queue_pressure: queue_length / (people_count + 1)
    """
    df = df.copy().sort_values("timestamp").reset_index(drop=True)
    df["rolling_avg_people"] = df["people_count"].rolling(5, min_periods=1).mean()
    df["entry_velocity"] = df["people_count"].diff().fillna(0)
    df["queue_pressure"] = df["queue_length"] / (df["people_count"] + 1)
    return df


def train() -> dict:
    """
    Train models from CSV data.
    Returns status dict with 'model_status' and optional 'error'.
    """
    global _model, _anomaly_model, _scaler, _is_ready, _train_error

    df = load_all_data()
    row_count = len(df)

    if row_count < MIN_DATA_THRESHOLD:
        _is_ready = False
        _train_error = f"Insufficient data: {row_count} rows (need ≥ {MIN_DATA_THRESHOLD})"
        return {"model_status": "not_ready", "error": _train_error, "rows": row_count}

    try:
        df = _engineer_features(df)

        feature_cols = ["rolling_avg_people", "entry_velocity", "queue_pressure", "people_count", "queue_length"]
        X = df[feature_cols].values
        y = df["wait_time"].values

        _scaler = StandardScaler()
        X_scaled = _scaler.fit_transform(X)

        _model = RandomForestRegressor(n_estimators=100, random_state=42)
        _model.fit(X_scaled, y)

        _anomaly_model = IsolationForest(contamination=0.1, random_state=42)
        _anomaly_model.fit(X_scaled)

        _is_ready = True
        _train_error = None
        return {"model_status": "ready", "error": None, "rows": row_count}

    except Exception as e:
        _is_ready = False
        _train_error = str(e)
        return {"model_status": "not_ready", "error": str(e), "rows": row_count}


def predict(people_count: int, queue_length: int) -> dict:
    """
    Predict wait time and detect anomaly.
    Returns error if model is not ready — NEVER guesses.
    """
    global _model, _anomaly_model, _scaler, _is_ready

    if not _is_ready or _model is None or _scaler is None:
        return {
            "model_status": "not_ready",
            "predicted_wait_time": None,
            "anomaly": None,
            "error": _train_error or "Model not trained yet",
        }

    try:
        df = load_all_data()
        df = _engineer_features(df)

        # Use last rolling stats to compute features
        if len(df) >= 1:
            last = df.iloc[-1]
            rolling_avg = last["rolling_avg_people"]
            entry_velocity = last["entry_velocity"]
        else:
            rolling_avg = people_count
            entry_velocity = 0

        queue_pressure = queue_length / (people_count + 1)

        X = np.array([[rolling_avg, entry_velocity, queue_pressure, people_count, queue_length]])
        X_scaled = _scaler.transform(X)

        predicted_wait = float(_model.predict(X_scaled)[0])
        anomaly_score = _anomaly_model.predict(X_scaled)[0]  # -1 = anomaly, 1 = normal
        is_anomaly = anomaly_score == -1

        return {
            "model_status": "ready",
            "predicted_wait_time": round(predicted_wait, 2),
            "anomaly": is_anomaly,
            "error": None,
        }

    except Exception as e:
        return {
            "model_status": "error",
            "predicted_wait_time": None,
            "anomaly": None,
            "error": str(e),
        }


def get_model_status() -> str:
    """Return 'ready' or 'not_ready'."""
    return "ready" if _is_ready else "not_ready"
