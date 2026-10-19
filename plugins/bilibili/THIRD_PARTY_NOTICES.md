# Third-party notices

This package adapts the read-only Bilibili subtitle/search workflow and optional local ASR approach from:

- **dsh-bili-scroll**, version 0.1.10, commit `5a2081bfcd67a4a3e9f24948a8449d184eb70ece`, https://github.com/c1overzz/dsh-bili-scroll
  — MIT, copyright (c) 2026 dsh-bili-duty contributors. See [licenses/dsh-bili-scroll-MIT.txt](licenses/dsh-bili-scroll-MIT.txt).
- **bilibili-mcp**, as vendored in the above commit under `vendor/bilibili-mcp/`
  — MIT, copyright (c) 2026 AGI_Ananas. See [licenses/bilibili-mcp-MIT.txt](licenses/bilibili-mcp-MIT.txt).

The DesktopPet integration replaces the DSH host, scheduler, duty/report workflow and whole-transcript output with the existing capability contract, the subject's activity selection and bounded timed-text pages. No upstream platform-write tools are included.

Network and optional ASR dependencies are installed separately, not vendored here. Their own license files remain with their installed distributions.
