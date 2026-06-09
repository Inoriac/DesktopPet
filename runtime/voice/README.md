# Optional voice runtime assets

This directory is intentionally kept out of Git except for this README.

Place or download GENIE / genie-tts runtime files here:

- `GenieData/`: GENIE base resources (`GENIE_DATA_DIR`).
- `CharacterModels/`: predefined speakers loaded by `genie.load_predefined_character`, such as `feibi`, `mika`, and `thirtyseven`.
- `custom_characters/<name>/tts_models/`: custom converted ONNX models.
- `custom_characters/<name>/reference.wav`: optional reference audio for custom voice style.
- `outputs/`: optional generated audio when `saveAudio` is enabled.

Use `tools/voice/setup_voice_env.ps1` to create the Python virtual environment, then run `tools/voice/download_genie_assets.py` to prepare GENIE data and preset speakers.
