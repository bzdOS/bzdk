# attic/ — retired one-shot scripts

Dead ends and single-use diagnostics from the July 2026 bring-up, kept for
history but no longer part of any workflow. All of these were superseded by the
five canonical tools in the repo root:

- `reliable_load.py` — deterministic load state machine (catch U-Boot → TFTP →
  loady/bootelf → verify EMAC + breadcrumb).
- `supervise.py` — autonomous board supervisor / self-heal.
- `soak.py` — unattended soak / boot-cycle harness with a durable report.
- `chimpd.py` — U-Boot catch + TFTP + loady loop.
- `hvdbg.py` — host-side EMAC debug-protocol client library.

Nothing in the live tree imports anything here (these are standalone `python3
foo.py` one-shots; the hyphenated names aren't even importable as modules). If
you need one, run it from here or lift the relevant snippet into a canonical
tool. Also holds two retired one-shot C diagnostics (`main_diag.c`,
`emmc_dma_probe.c`) that were never part of any Makefile target.
