# Quickstart

## 1. Install the Python SDK

```bash
python -m venv .venv
source .venv/bin/activate
pip install -e sdk/python
```

Windows PowerShell:

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -e sdk/python
```

## 2. Start the Linux reference device

```bash
python examples/linux/simulated_device.py
```

## 3. Discover and invoke from the DCO-Edge style client

```bash
python examples/dco-edge/discover_and_invoke.py
```

The client fetches `/manifest`, converts capabilities into agent-facing tools, then invokes `led.set` and `servo.move`.

## HTTP v0.1

- `GET /health`
- `GET /manifest`
- `POST /invoke`

Example:

```json
{
  "capability": "servo.move",
  "arguments": {"angle": 30}
}
```
