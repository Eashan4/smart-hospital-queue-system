/**
 * script.js — AIoT Smart Hospital Queue & Crowd Monitor
 * Polls /status every 4s. Drives ambient UI state, no fake data shown.
 */

"use strict";

// ── Config ──────────────────────────────────────────────────
const POLL_INTERVAL_MS  = 4000;
const HIGH_CROWD_THRESH = 30;     // people_count above which = crowd state

// ── DOM Refs ────────────────────────────────────────────────
const dom = {
  body:           document.body,
  alertBanner:    document.getElementById("alert-banner"),
  alertMsg:       document.getElementById("alert-msg"),
  noDataCard:     document.getElementById("no-data-card"),
  metricsGrid:    document.getElementById("metrics-grid"),
  lastUpdateTime: document.getElementById("last-update-time"),
  rowCount:       document.getElementById("row-count"),
  thresholdWarn:  document.getElementById("threshold-warn"),

  // Status panel
  dotDevice:      document.getElementById("dot-device"),
  txtDevice:      document.getElementById("txt-device"),
  dotModel:       document.getElementById("dot-model"),
  txtModel:       document.getElementById("txt-model"),

  // Cards
  valQueue:       document.getElementById("val-queue"),
  subQueue:       document.getElementById("sub-queue"),
  cardQueue:      document.getElementById("card-queue"),

  valCrowd:       document.getElementById("val-crowd"),
  subCrowd:       document.getElementById("sub-crowd"),
  cardCrowd:      document.getElementById("card-crowd"),

  valWait:        document.getElementById("val-wait"),
  subWait:        document.getElementById("sub-wait"),
  cardWait:       document.getElementById("card-wait"),

  valPatient:     document.getElementById("val-patient"),
  cardPatient:    document.getElementById("card-patient"),

  anomalyBadge:   document.getElementById("anomaly-badge"),
};

// ── State ────────────────────────────────────────────────────
let currentState = "normal";   // normal | crowd | anomaly | offline

// ── Helpers ──────────────────────────────────────────────────
function setBodyState(state) {
  if (currentState === state) return;
  currentState = state;
  dom.body.className = `state-${state}`;
}

function showAlert(msg) {
  dom.alertBanner.classList.add("show");
  dom.alertMsg.textContent = msg;
}

function hideAlert() {
  dom.alertBanner.classList.remove("show");
}

function showNoData(reason) {
  dom.noDataCard.classList.add("show");
  dom.metricsGrid.style.opacity = "0.25";
  document.getElementById("no-data-reason").textContent = reason;
}

function hideNoData() {
  dom.noDataCard.classList.remove("show");
  dom.metricsGrid.style.opacity = "1";
}

function setCardSkeleton(card, valueEl) {
  valueEl.classList.add("skeleton");
  valueEl.textContent = "——";
  card.classList.remove("card-active", "card-anomaly", "card-crowd-high");
}

function setCardValue(valueEl, text, isNA = false) {
  valueEl.classList.remove("skeleton");
  valueEl.textContent = text;
  valueEl.className = isNA ? "card-value value-na" : "card-value";
}

function setDeviceStatus(online) {
  if (online) {
    dom.dotDevice.className = "dot-pulse dot-online";
    dom.txtDevice.className = "status-value status-online";
    dom.txtDevice.textContent = "Online";
  } else {
    dom.dotDevice.className = "dot-pulse dot-offline";
    dom.txtDevice.className = "status-value status-offline";
    dom.txtDevice.textContent = "Offline";
  }
}

function setModelStatus(ready) {
  if (ready) {
    dom.dotModel.className = "dot-pulse dot-online";
    dom.txtModel.className = "status-value status-online";
    dom.txtModel.textContent = "Ready";
  } else {
    dom.dotModel.className = "dot-pulse dot-warning";
    dom.txtModel.className = "status-value status-warning";
    dom.txtModel.textContent = "Not Ready";
  }
}

function setAnomalyBadge(anomaly) {
  const badge = dom.anomalyBadge;
  if (anomaly === null || anomaly === undefined) {
    badge.className = "anomaly-badge anomaly-na";
    badge.innerHTML = "⬜ No anomaly data";
    return;
  }
  if (anomaly === true) {
    badge.className = "anomaly-badge anomaly-true";
    badge.innerHTML = "🔴 Anomaly Detected";
  } else {
    badge.className = "anomaly-badge anomaly-false";
    badge.innerHTML = "🟢 Normal Pattern";
  }
}

function stampTime() {
  const now = new Date();
  dom.lastUpdateTime.textContent = now.toLocaleTimeString();
}

// ── Main Poll ────────────────────────────────────────────────
async function fetchStatus() {
  try {
    const res  = await fetch("/status");
    const data = await res.json();

    stampTime();

    const deviceOnline = data.device_status === "online";
    const modelReady   = data.model_status  === "ready";

    setDeviceStatus(deviceOnline);
    setModelStatus(modelReady);

    // ── Device offline → grey state, no-data shown ───────────
    if (!deviceOnline) {
      setBodyState("offline");
      showNoData("Waiting for device…");
      showAlert(data.error || "Device is offline — no sensor data received.");
      // Skeleton all cards
      [dom.valQueue, dom.valCrowd, dom.valWait].forEach(el => el.classList.add("skeleton"));
      dom.valPatient.textContent = "—";
      setAnomalyBadge(null);
      return;
    }

    hideAlert();
    hideNoData();

    // ── Queue Length ─────────────────────────────────────────
    if (data.queue_length !== null && data.queue_length !== undefined) {
      setCardValue(dom.valQueue, data.queue_length);
      dom.cardQueue.classList.add("card-active");
    } else {
      setCardValue(dom.valQueue, "No Data", true);
      dom.cardQueue.classList.remove("card-active");
    }

    // ── People Count / Crowd ─────────────────────────────────
    if (data.people_count !== null && data.people_count !== undefined) {
      setCardValue(dom.valCrowd, data.people_count);
      const highCrowd = data.people_count > HIGH_CROWD_THRESH;
      dom.cardCrowd.classList.toggle("card-crowd-high", highCrowd);

      if (!highCrowd) dom.cardCrowd.classList.add("card-active");
      else            dom.cardCrowd.classList.remove("card-active");

      if (highCrowd && !data.anomaly) setBodyState("crowd");
    } else {
      setCardValue(dom.valCrowd, "No Data", true);
      dom.cardCrowd.classList.remove("card-active", "card-crowd-high");
    }

    // ── Predicted Wait Time ──────────────────────────────────
    if (!modelReady) {
      setCardValue(dom.valWait, "Model Not Ready", true);
      dom.cardWait.classList.remove("card-active");
    } else if (data.predicted_wait_time !== null && data.predicted_wait_time !== undefined) {
      setCardValue(dom.valWait, `${data.predicted_wait_time} min`);
      dom.cardWait.classList.add("card-active");
    } else {
      setCardValue(dom.valWait, "No Prediction", true);
      dom.cardWait.classList.remove("card-active");
    }

    // ── Current Patient ──────────────────────────────────────
    dom.valPatient.textContent = data.current_patient ?? "—";

    // ── Anomaly ──────────────────────────────────────────────
    setAnomalyBadge(data.anomaly);
    if (data.anomaly === true) {
      setBodyState("anomaly");
      dom.cardWait.classList.add("card-anomaly");
      showAlert("⚠️ Anomaly detected in crowd pattern — please investigate.");
    } else {
      dom.cardWait.classList.remove("card-anomaly");
      if (currentState === "anomaly") setBodyState("normal");
    }

    // Default state if no special condition
    if (currentState !== "anomaly" && currentState !== "crowd") {
      setBodyState("normal");
    }

    // ── Error from backend (partial) ─────────────────────────
    if (data.error && data.error !== null) {
      if (!data.anomaly) showAlert(`ℹ️ ${data.error}`);
    }

  } catch (err) {
    // Network-level failure
    setBodyState("offline");
    showNoData("Cannot reach server");
    showAlert("🔌 Cannot connect to backend — server may be offline.");
    stampTime();
  }
}

// ── Row count poll ───────────────────────────────────────────
async function fetchRowCount() {
  try {
    const res  = await fetch("/history?limit=1");
    const data = await res.json();
    const count = data.count ?? 0;
    const min   = 10; // matches server MIN_DATA_THRESHOLD default

    dom.rowCount.textContent = count;
    if (count < min) {
      dom.thresholdWarn.textContent = ` — need ${min - count} more rows to train ML`;
      dom.thresholdWarn.style.display = "inline";
    } else {
      dom.thresholdWarn.style.display = "none";
    }
  } catch (_) {
    dom.rowCount.textContent = "?";
  }
}

// ── Init ─────────────────────────────────────────────────────
(function init() {
  // Skeleton cards on load
  [dom.valQueue, dom.valCrowd, dom.valWait].forEach(el => {
    el.classList.add("skeleton");
    el.textContent = "——";
  });

  fetchStatus();
  fetchRowCount();
  setInterval(fetchStatus,   POLL_INTERVAL_MS);
  setInterval(fetchRowCount, POLL_INTERVAL_MS * 3);

  document.getElementById("refresh-btn")
    .addEventListener("click", () => { fetchStatus(); fetchRowCount(); });
})();
