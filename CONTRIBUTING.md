# Contributing

- Edit this snapshot's kernel directly in `kernel/`. Do not run `xnu/apply.sh`
  or replace it with an unrelated upstream tree.
- Follow the Linux build stages in `README.md`. Verify components with
  `python3 scripts/verify-products.py --complete` and host tests with
  `bash scripts/test-host.sh`. Component compilation does not verify hardware.
- Describe what was tested, on which device variant, and what remains untested.
  Share a minimal redacted diagnostic only when needed; do not commit logs.
- Keep SDKs, system files, firmware, NV/calibration, certificates, tokens and
  device identifiers outside the published tree. Run
  `python3 scripts/audit-export.py` on a clean source export before publication;
  its reports are generated under ignored `build/reports/source-audit/`.
- Preserve upstream notices. Kernel changes retain applicable APSL/per-file
  terms; project-owned non-kernel contributions use MIT. Add new third-party
  origins and licenses to `NOTICE` and `THIRD_PARTY_LICENSES.md`.
- Keep the Bluetooth source provenance and license information in `NOTICE`
  accurate. Do not substitute a directory-wide license assumption.
