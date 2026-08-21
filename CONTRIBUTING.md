# Contributing

Thanks for taking an interest in this project. Issues and pull requests are welcome.

For anything substantial, please open an issue first so we can agree on the direction
before you spend time on it.

## Getting set up

The repository has three independent parts, and most changes touch only one:

| Part | Path | What you need |
|---|---|---|
| Dashboard | `index.html` | A browser. Open the file directly — no build step. |
| Firmware | `firmware/esp_code/` | Arduino IDE + the ESP8266 board package |
| Backend | `server/` | Python 3.9+ |

### Backend

```bash
cd server
python -m venv venv && source venv/bin/activate    # Windows: venv\Scripts\activate
pip install -r requirements.txt
cp .env.example .env
```

Run the test suite before opening a pull request:

```bash
cd server
python -m unittest discover -s tests -v
```

The tests use Flask's test client and write to a temporary CSV, so they neither bind a
port nor modify the committed `data.csv`.

### Firmware

There is no CI job for the sketch — compiling it needs the full ESP8266 toolchain.
**Please verify `firmware/esp_code/esp_code.ino` compiles in the Arduino IDE before
submitting firmware changes**, and say so in the pull request.

Required libraries are listed in the [README](README.md#1--install-the-arduino-toolchain).

### Dashboard

`index.html` is deliberately a single self-contained file with no build step or bundler —
that is what keeps the deployment backendless. Please keep it that way: no npm, no
frameworks, no external assets beyond the MQTT client already loaded from a CDN.

## Conventions

- **Pin changes must be reflected in the README.** The pinout tables are the reference
  people actually wire against, and a stale table is worse than none.
- **Never commit `.env`** or real WiFi credentials. `.env.example` is the template.
- **Keep patient data out of payloads.** RFID UIDs are pseudonymous, not anonymous —
  see the README's security notes.
- Match the surrounding style. The firmware uses `clang-format` defaults; the Python
  follows PEP 8.

## Pull requests

1. Fork the repository and branch from `main`
2. Make your change, with tests where the backend is involved
3. Confirm CI passes
4. Open the pull request, describing what changed and how you verified it

## Reporting bugs

Please include the part affected (dashboard, firmware, or backend). For firmware issues,
Serial Monitor output at 115200 baud is almost always the fastest route to a diagnosis;
for dashboard issues, the browser console.
