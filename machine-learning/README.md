# Machine Learning

ML models for the biometric sensor data carried over the
[firmware/lora-testing](../firmware/lora-testing) LoRa link (e.g.
classification/anomaly detection on vitals, signal quality prediction).

Nothing here yet — no toolchain has been picked. When code lands, this
README should cover:

- Language/toolchain (Python + PyTorch/TensorFlow/scikit-learn, etc.) and
  how to set up the environment (`requirements.txt`/`pyproject.toml`, or
  equivalent)
- Where training/inference data comes from (live capture vs. recorded
  datasets) and how it's formatted
- How to train and how to run inference, and where models/checkpoints are
  stored (likely outside git — see below)

Add a `.gitignore` here for whatever toolchain gets picked (e.g.
`__pycache__/`, `.venv/`) plus large artifacts that shouldn't go in git
(datasets, trained model checkpoints) — the root `.gitignore` intentionally
only covers cross-cutting OS/editor files, not subsystem tooling.
