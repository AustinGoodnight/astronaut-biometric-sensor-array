# Signal Processing

Analysis of RF data captured from the [firmware/lora-testing](../firmware/lora-testing)
link (e.g. RSSI logs, packet timing, demodulated payloads).

Nothing here yet — no toolchain has been picked. When code lands, this
README should cover:

- Language/toolchain (Python, MATLAB, etc.) and how to set up the
  environment (`requirements.txt`/`pyproject.toml`, or equivalent)
- How data gets from the firmware/serial capture into this pipeline
- How to run the analysis and where output goes

Add a `.gitignore` here for whatever toolchain gets picked (e.g.
`__pycache__/`, `.venv/` for Python) — the root `.gitignore` intentionally
only covers cross-cutting OS/editor files, not subsystem tooling.
