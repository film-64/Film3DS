# Image-free hardware evidence subset

As of2026-11-14. Start at [the hardware handoff](../../CAMERA_HARDWARE_HANDOFF.md).

* `index.json`:31 capture/analysis directory records, including incomplete and
  rejected results; these are not31 successful measurements.
* `hardware-status.json`:scoped machine-readable conclusions and open questions.
* `evidence/`:unchanged CSV/TXT/JSON originals, historical reports/notes, model
  outputs, reference readings and selected public provenance.
* `omitted-images.json`:1,335 omitted pixel-file names/sizes/SHA256; no pixel bytes.
* `files-sha256.json`:this publication's text/source integrity manifest.
* `VALIDATION.md` and `validation/`:commands and results from this clean handoff.

The preserved `original-provenance/*sha256.json` and recovery receipts describe
the original local archives, including files deliberately omitted here. They are
not this subset's file manifest. `deployment-public.json` is a derived selection
of source/binary hashes, arguments and contract, with the original record's SHA.
Local device addresses and unrelated deployment/session files are not required.

`passed` in a copied report retains that report's original schema/meaning. Capture
integrity, stable hardware controls, model holdout success and absolute calibration
are different claims. In particular, Batch28 capture completes but its overall
fixed-model validation is false:15fps passes and5fps has a CCM configuration change.

Images were verified in the original local acquisition, then omitted for this
publication at the user's request. Readers can verify text hashes and rebuild
synthetic producer tests; they cannot independently rerun image-dependent hardware
statistics against missing images. Full capture/report verifiers are unchanged
and must reject the incomplete public `recovered/` subset.

No hardware model is presented as a certifiedISO/optical-shutter/dynamic-range
calibration. Failed and partial experiments are preserved rather than relabeled.
