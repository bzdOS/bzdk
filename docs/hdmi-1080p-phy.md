# Why 1080p (148.5 MHz) doesn't lock the HDMI PHY, and what this patch does about it

Status: **UNTESTED on real hardware.** Everything below comes from static
register-level analysis and host-side builds only — no board, no tty, no
`bzdctl.py`. The `HDMI_MODE_1080P` opt-in this patch adds has never been
booted. Treat this as "here is the best evidence I could gather without
touching the board", not "this is fixed".

## The symptom (recap from hdmi.h)

720p (74.25 MHz pixel clock, TMDS 742.5 Mbps) is confirmed working on real
hardware — DE2 → TCON1 → DW-HDMI → PHY → scanout, live-verified to a physical
monitor. A prior, uncommitted attempt at 1080p (148.5 MHz, TMDS 1.485 Gbps)
failed: PHY_STATUS bit7 (lock) stayed 0 while DE2/TCON1 were confirmed up, and
`hdmi_init()` timed out at the PHY stage. That attempt's code was never
committed, so it can't be diffed directly — this document reconstructs the
question from first principles against the *current* (720p-only) `hdmi.c`.

## What I checked

`hdmi.c`'s own header comment says every register/sequence is ported from the
known-good Allwinner A64 U-Boot driver, with file:line citations. That source
tree is present locally at `/opt/bzdos/build/u-boot/drivers/video/sunxi/`, so
the first step was verifying the port is actually faithful, not assumed:

- `sunxi_dw_hdmi.c` (`sunxi_dw_hdmi_get_divider`, `_phy_init`, `_phy_set`,
  `_pll_set`) — compared line-by-line against `hdmi.c`'s `pll_video_pick()`,
  `phy_init()`, `phy_set()`. **Bit-for-bit identical**, including the two
  hardcoded delay tables and the "Allwinner code doesn't fail on timeout
  either" comment. No divergence found here for any resolution.
- `lcdc.c` (`lcdc_init`, `lcdc_tcon1_mode_set`, `lcdc_enable`) — compared
  against `stage_tcon()`. Same clk-delay formula
  (`vfp+vsync+vbp-2`, clipped to 30), same H/V total/backporch packing, same
  `DIV_ROUND_UP(pll3_hz, pixelclock)` LCD1 divider. No divergence.
- `drivers/clk/sunxi-ng/ccu-sun50i-a64.c` — the A64's real PLL_VIDEO0
  constraints: `192 MHz..1008 MHz` output range (`hdmi.c`'s search loop uses
  `192000..912000` kHz, a slightly tighter but compatible bound), plain
  integer N/M with a *separate* hardware fractional-mode special-case
  reserved for exactly 270/297 MHz (bit24 = "integer, not fractional" when
  set — `CCM_PLL3_INTEGER` names it correctly).
- `drivers/gpu/drm/sun4i/sun8i_hdmi_phy.c` +
  `sun8i_hdmi_phy_clk.c` — the **modern**, in-tree, symbolically-named
  register driver for the same PHY silicon family (A64/H3/R40 all set
  `has_phy_clk = true` and share this design, per
  `sun8i_hdmi_phy.c:598-620` and the A64 hdmi-phy DT node's
  `compatible = "allwinner,sun50i-a64-hdmi-phy"`).
- `arch/arm64/boot/dts/allwinner/sun50i-a64.dtsi` — checked for a
  `max-tmds-clock` cap on the A64's `hdmi`/`hdmi_phy` nodes. **There is none.**
  Mainline considers the full DW-HDMI/PHY envelope (up to the 297 MHz
  `sunxi_dw_hdmi_mode_valid()` ceiling) available on A64, i.e. 1080p60 is
  *supposed* to be reachable on this SoC family, not a documented erratum.

## The arithmetic is identical for 720p and 1080p

`pll_video_pick()`/`sunxi_dw_hdmi_pll_set()`'s search (div 1..16, target
`pixel_clock*div` constrained to PLL_VIDEO0's valid range, best-fit N/M) was
run by hand for both targets:

| target pixel clock | best (m, n, div) | achieved PLL_VIDEO0 | diff |
|---|---|---|---|
| 74.25 MHz (720p, **working**) | m=8, n=99, div=4 | 297.000 MHz | 0 |
| 148.5 MHz (1080p, **failing**) | m=8, n=99, div=2 | 297.000 MHz | 0 |

**Both modes program PLL_VIDEO0 to the exact same 297 MHz, exactly.** The only
things that differ downstream are: the LCD1/TCON1 divider (4 vs 2, both from
the same `DIV_ROUND_UP` formula, both well inside the 4-bit `CCM_LCD1_M`
field), and which bucket `phy_set()`'s divider-selection table
(`sunxi_dw_hdmi_get_divider`: ≤27→11, ≤74.25→4, ≤148.5→2, else→1) selects —
"case 4" for 720p, "case 2" for 1080p. Since PLL_VIDEO0's lock is the thing
that's identical between the two, and 720p is proven to lock it fine, PLL3
itself is not a plausible root cause.

## Decoding "case 2" against the modern driver — the key finding

`phy_set()`'s per-bucket magic constants (`PHY_PLL`/`PHY_CLK`/`PHY_UNK1`/
`PHY_UNK2`) are undocumented Allwinner BSP values in both the old U-Boot
driver and this port — U-Boot's own comment says so verbatim ("we don't know
much about those magic numbers"). But the *modern* Linux driver
(`sun8i_hdmi_phy.c`'s `sun8i_h3_hdmi_phy_config()`) programs the **same
physical registers** (register offsets line up exactly: `ANA_CFG1_REG=0x20`
↔ `PHY_CTRL`, `ANA_CFG2_REG=0x24` ↔ `PHY_UNK1`, `ANA_CFG3_REG=0x28` ↔
`PHY_UNK2`, `PLL_CFG1_REG=0x2c` ↔ `PHY_PLL`, `PLL_CFG2_REG=0x30` ↔ `PHY_CLK`,
`PLL_CFG3_REG=0x34` ↔ `PHY_UNK3`, `ANA_STS_REG=0x38` ↔ `PHY_STATUS`) using
named bitfields instead of raw hex, and picks its own per-bucket tuning by the
*same* 27/74.25/148.5 MHz boundaries. That makes it possible to decode the old
hex blobs field-by-field and check them against a from-first-principles
computation of what the modern driver would program. I did this in Python
(shown in full below) for both the 720p bucket ("case 4") and the 1080p
bucket ("case 2"):

```python
def AMPCK(x): return x<<14
def EMPCK(x): return x<<11
def AMP(x):   return x<<7
def EMP(x):   return x<<4
def WIRE(x):  return x<<18
SDAEN, SCLEN = 1<<2, 1<<0
base = WIRE(0x3e0) | SDAEN | SCLEN

b_148 = base | AMPCK(7) | AMP(9)          # modern driver's <=148.5MHz bucket
# => 0x0f81c485
```

Result: **`b_148` computes to `0x0f81c485` — an EXACT match for U-Boot/
`hdmi.c`'s `case 2` `PHY_UNK2` constant (`0x0F81C485`).** Decoding the rest of
`case 2`'s `PHY_UNK1` (`0x8063a800`) the same way against
`ANA_CFG2`'s fields reproduces the modern driver's `M_EN | REG_DENCK |
REG_DEN | REG_CKSS(1) | REG_CSMPS(1) | REG_BIGSWCK | REG_BIGSW | REG_SLV(2)`
— again bit-for-bit. And `PHY_CLK`'s low nibble (`phy_div - 1` OR'd in) decodes
to the modern driver's `PREDIV(2)` field exactly, which is what
`sun8i_phy_clk_set_rate()` would itself compute for a 297 MHz parent divided
down to 148.5 MHz.

The interesting control is what happens when the *same* decode is run against
`case 4` (the bucket 720p — the **working** mode — actually uses): the modern
driver's `<=74.25MHz` bucket calls for `AMPCK(5) | AMP(7)`, but U-Boot's
`case 4` constant decodes to `AMPCK(7) | AMP(8)` — U-Boot reuses the exact
same `PHY_UNK1`/`PHY_UNK2` pair for both its "case 4" *and* "case 11" buckets,
i.e. **the old driver's 720p tuning is a coarser, less-exact approximation of
the modern per-bucket table than its 1080p tuning is.** Put plainly: the
register recipe this file uses for 1080p is, if anything, *more* faithful to
the reference implementation than the recipe already proven to work for
720p. That is evidence against "the case-2 magic numbers are simply wrong" —
if approximate numbers lock fine at 720p, exactly-correct numbers shouldn't
be *less* likely to lock at 1080p on the same silicon, everything else equal.

## What I did NOT find

No register width/overflow issue (1920/1080's TCON1 H/V total values, PHY
divider fields, DE2 `de_ui`/blender geometry — all checked, all comfortably
within their field widths). No missing step relative to U-Boot's sequence
(probe-time fixed `phy_init()` defaults, then per-mode `pll_video_pick()` +
`phy_set()`, then TCON1/LCD1 divider from the *already-updated* PLL3 value,
then the FC_INVIDCONF/timing writes, then the PHY tail — same order, same
registers, same bounded-wait discipline as the source it's ported from). No
A64-specific DT erratum capping `max-tmds-clock` below 297 MHz. No difference
in the PLL_VIDEO0 target frequency between the two modes (both 297 MHz,
exactly, per the hand-run search above).

## Leading open explanation

Given the above, the most defensible remaining explanation is that this is
either (a) a genuine analog/signal-integrity limit specific to this board,
cable, or silicon instance at the doubled TMDS bit rate (1.485 Gbps vs
742.5 Mbps is a materially harder analog target — tighter rise/fall times,
trace-length/impedance matching, connector quality — and "the register
recipe is correct on paper" does not guarantee a clean eye diagram on a given
physical assembly), or (b) the previously-tested, now-lost code differed from
what's in this file today in some way that has since been fixed as part of
general hardening work and just happened not to get re-tested at 1080p. I
cannot distinguish between these without running it on the board, which is
out of scope for this pass.

## What this patch actually changes

1. **`hdmi.h`: 1080p mode constants, opt-in only.** Building without
   `-DHDMI_MODE_1080P` (every existing build target) reproduces the 720p
   constants byte-for-byte — this is not a behavioural change to the
   hardware-verified path. Building *with* it selects the correct CEA-861
   mode-16 timing (1920x1080@60, 148.5 MHz, htotal 2200, vtotal 1125) so a
   future hardware test exercises the mode-generic pipeline code
   (`stage_clocks`/`stage_de2`/`stage_tcon`/`stage_hdmi_ctrl`/`stage_phy`/
   `stage_scanout` — none of which needed to change; they're already fully
   parameterized by the `HDMI_MODE_*` macros) against real 1080p timings
   instead of requiring hand-edited constants next time someone wants to try
   this.
2. **`hdmi.c` `stage_phy()`: an actual bounded lock-confirmation wait.** This
   is the one concrete, low-risk bug I found regardless of resolution: the
   *only* `wait_bits()` check on `PHY_STATUS` bit7 anywhere in the file runs
   inside `phy_init()`, **before** `phy_set()` reprograms the PLL for the real
   target mode. Nothing after `phy_set()` ever confirmed, with a bounded
   wait, that the *actual* per-mode PLL came up — the breadcrumb's stage-5
   "done" and "the PHY silently never locked at this mode" were
   indistinguishable without a live, separate `PHY_STATUS` re-read (which is
   apparently how the original 1080p failure was diagnosed: by hand, after
   the fact, not by the driver's own bounded-wait contract). Added a
   `wait_bits(PHY_STATUS, 0x80, 0x80, 50000)` right after `phy_set()`
   returns, latching the existing `bc_timeout(HDMI_STAGE_PHY)` /
   `HDMI_STAGE_TIMEOUT` path on failure — no new breadcrumb word, no new
   failure mode, and it costs ~0 extra time in the confirmed-working 720p
   case (by the time `phy_set()` returns, its own internal delays already
   total 100-200ms, so the PLL has long since either locked or not). This
   means the *next* real hardware attempt at 1080p will get an explicit,
   correctly-attributed timeout in the breadcrumb instead of a "looks like
   it finished" false-positive.
3. **`hdmi.c` `hdmi_demo()`: fixed a real, independent bug.** The demo's
   last line was a hardcoded string, `"mode: 1920x1080@60  pixclk=148.5MHz"`,
   left over from whatever the code looked like during the original 1080p
   attempt — except the file has shipped 720p-only since its very first
   commit (`a67511a`), so this line has been lying about the actual scanout
   mode this entire time. Replaced it with `fb_dec()` calls against the real
   `hdmi_width()`/`hdmi_height()`/`HDMI_MODE_PIXEL_CLOCK_HZ`, so it can't go
   stale again the next time the compiled-in mode changes.

## Build verification

```
cd /opt/bzdos/microkernel
LC_ALL=C make dbg EXTRA_CFLAGS="-DHV_HDMI"                      # default, 720p
LC_ALL=C make dbg EXTRA_CFLAGS="-DHV_HDMI -DHDMI_MODE_1080P"    # opt-in, 1080p
```

Both compile with **zero errors**. Both show the same two warnings the
unmodified tree already has before this patch
(`smp.c:363` unused-function, and the linker's RWX-segment-permissions
warning — verified identical via `git stash`/rebuild/`git stash pop`), so
this patch introduces **zero new warnings**.

## What remains UNVERIFIED

Everything about actual hardware behaviour. Specifically, unverified:

- Whether `-DHDMI_MODE_1080P` actually locks `PHY_STATUS` bit7 on this board.
  Given the analysis above, I genuinely don't know — the register recipe
  looks *more* correct than the working 720p recipe, which argues against a
  fixable software bug, but says nothing about the physical layer.
- Whether the new bounded post-`phy_set()` wait in `stage_phy()` ever
  actually latches a timeout on the *720p* path (it shouldn't, per the timing
  argument above, but this has not been run on the board since the change).
- Whether `hdmi_relock()` (which calls the same `phy_set()`) needs the same
  post-lock confirmation added — left alone in this patch since it already
  has its own PHY_STATUS re-read at the end of the function for its own
  breadcrumb word[3]; only `stage_phy()`'s boot-time path was missing a
  bounded check.
- Whether the DE2/TCON1 timing values for 1920x1080@60 (2200x1125 total)
  interact correctly with the DE2 mixer's own bandwidth/AXI limits at this
  resolution — the register fields all fit, but "fits in the field" is not
  the same as "the mixer can actually sustain that fetch rate", and that
  was not something I could check without live hardware.

If the next hardware test with `-DHDMI_MODE_1080P` still times out at
`HDMI_STAGE_PHY`, the breadcrumb (`BC_HDMI_BASE`, word[3]=`phy_status`,
word[4]=`timeout_stage`) will now say so explicitly rather than requiring a
manual live re-read — that's the concrete, checked-in improvement from this
pass, independent of whether 1080p itself turns out to be reachable on this
board.
