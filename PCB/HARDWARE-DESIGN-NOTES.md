# OpenAstroGuider PCB — hardware design reference

Working notes extracted from the three datasheets in this folder plus the SDK sources in this repo,
for the custom RV1106G3 + IMX290/IMX327 guide-camera board.

Sources, and how much to trust each:

| Source | Status |
|---|---|
| `IMX290_Datasheet.pdf` (IMX290LQR-C, Rev 0.4-ish, **Preliminary**) | Sony; several figures marked *tentative* |
| `IMX327LQR-C_TechnicalDatasheet_E_Rev0.2.pdf` (**Tentative**) | Sony |
| `RV1103 datasheet V1.5.pdf` | Rockchip **brief** datasheet (26 pp) — pin list + electricals only, **no TRM content** |
| This repo's kernel/clock/DTS sources | Ground truth for what the software actually expects |

Both Sony documents are for the **colour** (`-C`) parts. **Correction (2026-10-14): there is no
`IMX327LLR`.** Sony documents the IMX327 only as the colour LQR; no mono part was found in Sony's
catalogue or any distributor listing. The mono part in this package is the **`IMX290LLR`** (Sony
markets it as a monochrome industrial sensor). Package, pinout, rails and timing match the colour
parts; only the CFA differs.

Prices seen 2026-10-14: IMX290LQR-C **$12.02 @1 / $9.64 @90+** on JLC/LCSC (C5249315, out of stock,
"Standard PCBA only", X-ray required); IMX327LQR not on JLC/LCSC (~$11–15 marketplace, unverified);
IMX290LLR no trustworthy price — get a FRAMOS/Macnica quote.

Anything below marked **[OPEN]** could not be settled from these documents and needs the RV1106 TRM,
Rockchip's reference schematic, or a measurement.

Added 2026-10-14, in `reference/`: Luckfox's own **Pico Mini** and **Pico Plus** schematics (both
RV1103G1 + SPI NAND, from `github.com/LuckfoxTECH/Luckfox-Pico-docs`, `Hardware/Schematic/`) and the
**RV1106 datasheet V1.8**. The Luckfox schematics are the reference design — see §12.

---

## 0. Agreed baseline (2026-10-14, revised 2026-10-15)

Decided with the user; supersedes anything below that disagrees. Rows marked **(rev 10-15)** come
from the two-use-case revision — reasoning and circuit detail in §16.

| Item | Decision | Notes |
|---|---|---|
| Use cases **(rev 10-15)** | **One PCB, two assembly variants:** OAG (OpenAstroGuider, guiding + local hinted plate solving, mount-powered) and **Zenit** (remote camera for Zenit Astronomy: Wi-Fi to a main processor that runs as AP, battery-powered, no guiding, no dew heater) | Variant-only parts are DNP on the other variant; where the two variants need different extras they may share SoC pins (one net, two footprints, one populated). §16. |
| Outline **(rev 10-15)** | **50 × 50 mm, corners trimmed to a Ø56.5 mm circle** (2276 mm², +42 % over 40 × 40) | Must slide into the OAG tube (OD 59 mm); Ø56.5 is the old 40 × 40 diagonal, so the tube wall gets no thinner. Straight edges ~26 mm. **Mounting holes stay on the 30 × 30 square** (M2.5); move to a slightly larger square only if they block a lot. Zenit has no size constraint and uses the same board. |
| Power input **(rev 10-15)** | **USB-C VBUS is the only 5 V entry. No diode-OR, no power mux.** | Mount power arrives through the same USB-C (custom cable, below). The Zenit battery board feeds the same 5 V node and owns all reverse-current/back-feed protection (§16.2). Dew heater from a PC port can exceed 500 mA — software rule, not hardware. |
| Battery **(rev 10-15)** | **External battery PCB, not designed now** | Main board only provides a connector: 5 V, GND, POWER_HOLD, BAT_SENSE, CC1/CC2, optional BUTTON (§16.2). Runtime target 8 h min, 12 h+ preferred. |
| Mount link **(rev 10-15)** | **Custom cable: OAE mount XH-4 (5 V, GND, GPIO21, GPIO22) → USB-C plug.** 5 V/GND on VBUS/GND, data **half-duplex on SBU1+SBU2 tied together** (orientation-independent), CC pull-up in the plug | Camera is the only initiator (LX200 + polling), ESP32 only answers. Carries guide pulses, mount telemetry and the tunnelled PHD2 event-server traffic for N.I.N.A. (§16.3). |
| J203 **(rev 10-15)** | **Split (user, 2026-10-15): ST-4 on a 5-pin JST-GH (3.3 V, ST-4 ×4), UART on a separate 3-pin JST-GH (TX, RX, GND); both unpopulated in v1.** Replaces the 2.54 mm 1×8 header. TX/RX are **reserved for other mounts** (§16.7); their 5 V-tolerant protection (§16.4) is laid out but not populated | OAE no longer uses J203 — it connects through USB-C SBU (§16.3). The UART needs GND as its reference (the other device has its own supply); ST-4 does not (opto-isolated). RS-232 mounts via an external MAX3232 cable. |
| SoC | **RV1103G1** (QFN88, 64 MB DDR2) | Chosen on cost (~half an RV1106G2). **Not footprint-compatible with RV1106G2/G3** — those are QFN128 12.3 mm with 1.35 V DDR3L (§4). G2↔G3 *are* interchangeable with each other. Moving to 256 MB later means a new layout. |
| Sensor | **One footprint for IMX327LQR, IMX290LQR and IMX290LLR** | Leave C1/C2, D1/D2, K1/K2, L1/L2 **floating** (§1). Pick the part at order time. The mono IMX290LLR is the best guider (no CFA: more signal, no 2×2 pattern on few-pixel stars) but has no reliable price yet and needs a driver port. There is **no IMX327 mono part** (§1). |
| Boot / storage | **SPI NAND, 1 Gbit**, on FSPI | Restores maskrom/USB flashing. No SD slot. W25N02KV (2 Gbit) / W25N04KV (4 Gbit) are the same WSON-8 footprint and are in Rockchip's U-Boot `sfc_nand.c` ID table (`EF AA 22`/`23`) — upsizing is a BOM change. Plate solving needs ~12 MB (`PlateSolving/README.md`). |
| USB | **USB-C, USB 2.0, bus-powered**, on the side **opposite the sensor** | **(rev 10-15)** Also the mount-power + mount-data entry: SBU1/SBU2 (A8/B8, NC in schematic v1) now carry the half-duplex mount UART (§16.3). |
| Assembly | **Double-sided**: sensor (+ its decoupling + NTC) on the front, everything else on the back | Single-sided is not practical with USB-C opposite the sensor. Keep the SoC as far from the sensor as the board allows (dark current). |
| Production volume | **50–100 boards per order**, target ~$35/board incl. PCB + assembly | Minimise unique *extended* JLC parts; reuse values. |
| Layers | **4** | Sensor LGA interior power pins, MIPI reference plane, ~7 rails (§12). |
| Dew heater | **JST-XH 2-pin, 20 Ω strip at 5 V = 250 mA / 1.25 W, on/off only** | Low-side N-FET, **gate pull-down so it is off during boot**. Existing `OAG_DEWHEATER_GPIO` software works unchanged. |
| Temperature | **NTC + 1 resistor on SARADC_IN1**, next to the sensor | Not IN0 — that is the recovery key (§12.4). |
| Recovery | **Pads, no buttons**: SARADC_IN0 recovery pad + debug-UART pads | Dev boards will see many bad images; may be depopulated for production if space is needed (§12.4). |
| LEDs **(rev 10-15)** | ~~None~~ → **one error LED, both variants** | Off in normal operation; blink codes only on failure (Wi-Fi, mount link). Faces away from the sensor (light leaks). |
| Button **(rev 10-15)** | **Zenit only**, on the battery board (power on), copied to the SoC as BUTTON | Long press = re-pair Wi-Fi (proposal). |
| ST-4 **(rev 10-15)** | **Not in v1 — exposed and reserved.** ST-4 GPIOs 8/9/10/12 + 3.3 V on their own **5-pin JST-GH, unpopulated in v1** (not a 2.54 header); the RJ12 and optos live on a separate retrofittable ST-4 extension board | Interface contract: the kit wires its opto LEDs **active-low** (anode on 3.3 V, cathode via R into the GPIO), because these pins default high at boot (§13) — so no stray guide pulses during boot. §16.6. |
| Sensor sync | **Slave mode routed in v1** (XVS/XHS/XMASTER on VCCIO6 at 1.8 V) | So the board can be a full astro camera. Sony guarantees only ~1 s exposures in either mode (§6). |
| Library parts | RV1103G1 C5307358 · EA3036CQBR C570857 · W25N01KVZEIR C22472329 · IMX290LQR-C C5249315 | All in the EasyEDA system library with symbol + footprint. IMX290LLR is not listed — same LGA-110 footprint, use the LQR symbol. A community IMX327LQR_C entry uses the same footprint. |
| Wi-Fi **(rev 10-15)** | ~~Not in v1~~ → **SDIO module on SDIO_M1 (pins 60–65), chip antenna** (3D-printed enclosure, no U.FL needed) | **OAG: only for standalone operation** (phone, no PC) — never in the guiding/dither/N.I.N.A. control path. **Zenit: the primary data link.** Module must support **1.8 V SDIO** (VCCIO6) and its **32.768 kHz LPO** need must be checked; chip must have a driver in this SDK (aic8800dc, rtl8723ds, rtl8189fs, bcmdhd/cywdhd, …). §16.5. |
| Lens mount | **None on the PCB** — built into the OpenAstroGuider enclosure | Board dimensions: see Outline above. |

---

## 1. Sensor choice: IMX290 vs IMX327

They are the same silicon family in the same package with the same control model. Differences that
matter here:

| | IMX290LQR | IMX327LQR |
|---|---|---|
| Max 1080p frame rate | 120 fps | 60 fps |
| Analog gain | 0–30 dB (+33 dB digital) | 0–27 dB (+42 dB digital) |
| Output options | CMOS parallel, LVDS (8 ch), CSI-2 | LVDS (4 ch), CSI-2 |
| Kernel driver in this tree | `imx290.c` — **mainline** (FRAMOS/Linaro), no Rockchip glue | `imx327.c` — **Rockchip vendor** driver, full `rkmodule` integration, MIPI *and* LVDS modes |

For a guide camera the frame-rate advantage is irrelevant, so **driver status is the deciding factor**.
`imx327.c` is a complete vendor driver with the same register model the SC3336 work already uses;
`imx290.c` is the upstream driver and would need rkcif/rkmodule plumbing. Register sets are
near-identical, so `imx327.c` can also be retargeted at an IMX290 with modest edits.

Both drivers hardcode `MEDIA_BUS_FMT_SRGGB10_1X10` / `SRGGB12_1X12`. **A mono part needs the format
changed to `Y10_1X10`/`Y12_1X12`** and `sensor_desc_t.bayer` in `alpaca/src/sensor.h` set to mono —
already supported by that abstraction.

### Pin compatibility between the two

**The CSI-2 side is pin-identical.** The CSI-2 pins (N5–N9, P5–P9), all power/ground, `INCK`, and every
control pin are at the same positions on both parts.

The **only** difference is columns C, D, K, L in rows 1–2:

| Pins | IMX327 | IMX290 |
|---|---|---|
| C1/C2, D1/D2, K1/K2, L1/L2 | `N.C.` (GND-connectable) | `DLOMA/DLOPA`, `DLOMB/DLOPB`, `DLOMG/DLOPG`, `DLOMH/DLOPH` — **outputs** |

> **Design rule:** if you want one footprint to accept either part, leave C1/C2/D1/D2/K1/K2/L1/L2
> **floating**. Grounding them (legal on IMX327) would short IMX290 output drivers.

Also note E1/E2, F1/F2, H1/H2, J1/J2 carry LVDS outputs on both parts but with *different lane letters*
(`DLOMA..D` on IMX327 vs `DLOMC..F` on IMX290). Irrelevant if you use CSI-2 and leave them open.

---

## 2. Sensor electrical

### 2.1 Rails

| Rail | Pins | Min / Typ / Max |
|---|---|---|
| **AVDD 2.9 V** (analog) | A4, A6, A7, A8, A9 (`VDDHAN`/`VDDHPX`/`VDDHCP`) | 2.80 / 2.90 / 3.00 V |
| **OVDD 1.8 V** (interface) | M1 (`VDDMIF`) | 1.70 / 1.80 / 1.90 V |
| **DVDD 1.2 V** (digital) | D11, M11 (`VDDLCN`); E4, E8, K5, K8 (`VDDLSC`); K6, K7 (`VDDLIF`) | 1.10 / 1.20 / 1.30 V |

Absolute max: AVDD/OVDD 3.3 V, DVDD 2.0 V. Input voltage on any digital pin: −0.3 V to **OVDD + 0.3 V**,
never above 3.3 V. Operating guarantee −30 to +85 °C; *performance* guarantee only −10 to +60 °C.

**Current consumption is `TBD` in both datasheets** — Sony never filled the table in these revisions.
Budget from module-vendor figures or measure a dev module; do not guess from the datasheet.

### 2.2 Decoupling (from the IMX290 "Peripheral Circuit" page, which is the more explicit of the two)

Each supply pin gets its own **pair**:

| Pin(s) | Caps, per pin |
|---|---|
| A7 `VDDHCP` | 0.1 µF ‖ 1.0 µF |
| A4, A6, A8, A9 `VDDHAN`/`VDDHPX` | 0.1 µF ‖ 4.7 µF each |
| D11, M11 `VDDLCN` | 0.1 µF ‖ 4.7 µF each |
| E4, E8, K5, K8 `VDDLSC` | 0.1 µF ‖ 4.7 µF each |
| K6, K7 `VDDLIF` | 0.01 µF ‖ 1.0 µF each |
| M1 `VDDMIF` | 0.01 µF ‖ 4.7 µF |

That is 12 pairs = **24 capacitors** for the sensor alone. The IMX327 page shows a slightly looser
variant (one 10 µF bulk per rail + single caps); the IMX290 arrangement is a superset — follow it.

### 2.3 Reference pins — caps to GND, not supplies

| Pin | Symbol | Cap |
|---|---|---|
| A3 | `VLOADLM` | 1.0 µF |
| A5 | `VRLFR` | 1 µF |
| B5 | `VRLST` | 1 µF |
| C11 | `VBGR` | 0.22 µF |

Leave **open**: C10 `TAMON`, J11 `TOUT`, E11 `TENABLE`, and (in I²C mode) F10 `SDO`.

### 2.4 Control-pin strapping

| Pin | Symbol | For this design |
|---|---|---|
| J10 | `OMODE` | **Low = CSI-2** (High = LVDS). Strap low, or drive from a GPIO if you want to keep LVDS as a fallback. |
| F11 | `XMASTER` | **Low = master mode**, High = slave mode. Must be stable *before* reset is released and must not change during operation. Bring it to a GPIO — see §7. |
| K10 | `XCLR` | System clear, active low. Drive from a SoC GPIO. |
| K11 | `XTRIG` | Leave open. |
| G11 | `XCE` | In I²C mode, **tie to OVDD** (1.8 V). |
| G10 | `SCK` → SCL | 1 kΩ pull-up to OVDD (Sony's own value — note this is much stiffer than the usual 2.2–10 k). |
| H10 | `SDI` → SDA | 1 kΩ pull-up to OVDD. |
| H11 / L10 | `XVS` / `XHS` | Bidirectional: outputs in master mode, inputs in slave mode. See §7. |
| P3 | `INCK` | 37.125 MHz or 74.25 MHz, 45–55 % duty, at OVDD levels. |

**I²C address is a fixed 7-bit `0x1A`** on both parts — no strap, so two of these cannot share a bus
without a mux. (The datasheet's "I²C pin description" table claims SCL=G2 / SDA=E2; that contradicts
both the pin table and the application circuit, which say **G10 / H10**. Trust G10/H10.)

### 2.5 Power sequencing — this constrains the regulator design

**On:** DVDD 1.2 V → OVDD 1.8 V → AVDD 2.9 V, all rails up within **200 ms**.
**Off:** exact reverse, all down within 200 ms.

Then: `XCLR` low ≥ 500 ns after all rails are up and INCK is stable; `XCE` must rise *after* OVDD;
≥ 20 µs from `XCLR` high to first I²C transaction.

Every digital input (`INCK`, `XCE`, `SCK`, `SDI`, `XCLR`, `XMASTER`, `OMODE`, `XVS`, `XHS`) must be at
0 V *before* OVDD falls.

> Implication: you need **sequenced enables** on the three sensor rails (a 3-channel load switch chain,
> a sequencing PMIC, or RC-delayed enables), not three free-running LDOs. The reverse-order power-down
> requirement is the awkward half — a plain "cut input power" shutdown violates it.

The Rockchip driver models these as regulators named `avdd` / `dovdd` / `dvdd` (mainline `imx290.c`
uses `vdda` / `vddd` / `vdddo`), so GPIO-controlled enables map cleanly onto DT `regulator-fixed` nodes.

---

## 3. Sensor mechanical / optical

- Package: **110-pin ceramic LGA**, gold-plated, **12.00 ± 0.10 mm × 9.30 ± 0.10 mm**, 0.8 mm pitch,
  Ø0.5 ± 0.07 mm pads. Body height 1.52 ± 0.20 mm.
- **Optical centre** relative to package references B/B′: **(H, V) = (6.058, 4.415) ± 0.075 mm**.
  Package centre is (6.00, 4.65), so the optical axis sits **+0.058 mm in H and −0.235 mm in V** off
  package centre. Do not centre the lens on the package outline.
- Image-area rotation tolerance **±1°**; tilt relative to package bottom **< 0.05 mm**.
- **Height from package bottom "C" to the sensor plane: 0.62 ± 0.1 mm.**
- **Height from the top of the cover glass "D" to the sensor plane: 0.90 ± 0.15 mm.**
- **Cover glass: 0.50 mm thick, n = 1.5.** Plain glass — no integrated IR-cut, which is what you want.
  It shifts focus by (n−1)/n × t ≈ **0.167 mm**; fold that into the back-focal calculation for the
  M12/C-mount holder.
- Effective pixels 1945 × 1097; recommended recording 1920 × 1080; **2.9 µm square pixels**;
  Type 1/2.8, 6.46 mm diagonal (5.57 × 3.13 mm active).

The ±0.075 mm optical-centre tolerance and ±1° rotation are the dominant mechanical error budget —
the lens holder's datum should reference the package, and the holder-to-PCB registration wants to be
tighter than that.

---

## 4. RV1103 package and pin geography

> **Correction (2026-10-14):** this section is **RV1103 only**. The RV1106G2/G3 are **QFN128,
> 12.3 × 12.3 mm**, with **DDR3L at 1.35 V** (RV1103: DDR2 at 1.8 V) and a different pin numbering
> (RV1106 datasheet V1.8, `reference/`). The earlier assumption that they share the QFN88 family was
> wrong. G2 and G3 share one pin table and are interchangeable with each other.

**QFN88, 9 × 9 mm body, 0.35 mm pitch**, exposed pad 6.5 × 6.5 mm (6.4–6.6), lead width 0.17 mm,
lead length 0.4 mm, total height 0.9 mm, coplanarity 0.08 mm. MSL3, Sn finish.

0.35 mm pitch with a 6.5 mm thermal pad on a 9 mm body means **fan-out needs microvias or a very
careful 4-layer stack-up**; the exposed pad has to carry the thermal load (it is the only VSS).

Pin 1 is at the bottom-left in the bottom view, numbering counter-clockwise, 22 pins per side:

| Edge (bottom view) | Pins | Contents |
|---|---|---|
| **Left** | 1–22 | MIPI_CLK0_OUT (1), GPIO3_C5 (2), MIPI_CLK1_OUT/PWM7 (3), I2C4_M2 (4–5), DVDD_1 (6), VCCIO4 (7), **SDMMC** (8–14), SARADC (15–16), SARADC_USB_AVDD1V8 (17), **USB_DM/DP (18–19)**, USB_AVDD3V3 (20), CODEC out (21–22) |
| **Top** | 23–44 | CODEC (23–26), GPIO3_VCC (27), **FSPI** (28–33), **DDR** (34–40), TVSS (41), PWR_CTRL (42), PMU_VCC3V3 (43), OSC_PLL_PMU_DVDD (44) |
| **Right** | 45–66 | **24 MHz XIN/XOUT (45–46)**, OSC/PLL_AVDD1V8 (47), CPU_AVS (48), JTAG/UART2 (49–50), VCCIO1 (51), GPIO1_C6/C7 (52–53), I2C3_M1 (54–55), GPIO1_D0/D1 (56–57), DVDD_4 (58), VCCIO6 (59), **SDIO** (60–65), OTP/ETH/TSADC_AVDD1V8 (66) |
| **Bottom** | 67–88 | **FEPHY (67–72)**, DVDD_5 (73), **CPU_DVDD (74)**, DVDD_6 (75), **MIPI CSI D3/CK1/D2/D1/CK0/D0 (76–87)**, MIPI_AVDD1V8/VCCIO7 (88) |

**The single most useful layout fact:** the MIPI lanes occupy pins 76–87 on the bottom edge, and pins
1–5 (sensor MCLK, a spare PWM-capable GPIO, and I²C4) are immediately around the pin-1/pin-88 corner.
**The whole camera interface lives in one corner of the package** — put the sensor there and the
differential pairs stay short and un-crossed.

### 4.1 Supply rails to plan for

| Voltage | Pins |
|---|---|
| 0.85–1.05 V `CPU_DVDD` (DVFS domain, separate) | 74 |
| 0.9 V `DVDD_1..6` logic | 6, 36, 39, 58, 73, 75 |
| 0.9 V `OSC_PLL_PMU_DVDD` | 44 |
| 1.8 V `DDR_VDDQ_1..3` | 34, 35, 40 |
| 1.8 V `DDR_PLL_AVDD1V8` | 38 |
| 1.8 V `OSC_AVDD1V8/PLL_AVDD1V8` | 47 |
| 1.8 V `MIPI_AVDD1V8/VCCIO7_VCC1V8` | 88 |
| 1.8 V `SARADC_USB_AVDD1V8` | 17 |
| 1.8 V `OTP/ETH/TSADC_AVDD1V8` | 66 |
| 1.8 V `CODEC_AVDD1V8` | 23 |
| 3.3 V `USB_AVDD3V3` | 20 |
| 3.3 V `FEPHY_AVDD3V3` | 71 |
| **3.3 V only** `VCCIO1_VCC3V3`, `PMU_VCC3V3` | 51, 43 |
| **1.8 V *or* 3.3 V** `VCCIO4`, `VCCIO6`, (`GPIO3_VCC`) | 7, 59, 27 |
| Passive | `DRAM_ZQ` (37), `FEPHY_EXTR` (72), `TVSS` (41) |
| PMIC control | `PWR_CTRL_M0` (42) |

Note the pin *name* `VCCIO4_VCC3V3` says 3V3 but Table 3-2 lists `VCCIOi_VCC (i = 3, 4, 6)` as
1.8 V/3.3 V selectable. Trust the table; confirm against the TRM.

Unused blocks you can drop for this design: the FEPHY Ethernet (67–72) and the audio CODEC (21–26).
Both still need their AVDD pins handled per Rockchip's reference schematic even when unused. **[OPEN]**

**[OPEN]** `DRAM_ZQ` and `FEPHY_EXTR` resistor values, crystal load caps, and the disposition of unused
analog pins are **not in the brief datasheet**. Get Rockchip's reference schematic for these — they are
exactly the details that silently break a first spin.

---

## 5. SoC ↔ sensor interface

### 5.1 MIPI CSI-2

RV1103 has two 2-lane D-PHY v1.2 receivers at **1.5 Gbps/lane**, sharing the 12 pins at 76–87. They
combine into one 4-lane receiver. Verified in `drivers/phy/rockchip/phy-rockchip-csi2-dphy-hw.c`:

- **Full (4-lane) mode:** data lanes 0–3 → `MIPI_CSI_RX_D0..D3`, clock → **`CK0`** only. `CK1` unused.
- **Split mode:** `csi2_dphy1` = D0/D1 + CK0; `csi2_dphy2` = D2/D3 + CK1.

Sensor-side lane mapping is fixed by Sony: `DMO1`→Lane 1, `DMO2`→Lane 2, `DMO3`→Lane 3, `DMO4`→Lane 4,
`DMCK`→clock. No lane remap or polarity swap is exposed by the Rockchip PHY driver, so **route P→P,
N→N, in order**.

Bandwidth: 1080p30 RAW12 is 445.5 Mbps/lane on 2 lanes, 222.75 on 4. IMX290's absolute max is
891 Mbps/lane. All well inside the RV1103's 1.5 Gbps.

> **2 lanes is sufficient** for a guide camera — a 0.5 s exposure does not care about readout time.
> But the 4 lanes cost only 4 extra traces on dedicated pins, so route all four plus CK0 for headroom
> and to keep the option of a higher-frame-rate mode. Leave CK1 (78/79) unconnected.

Sony's application note asks for the differential pairs to be "as short as possible", matched, and with
the series termination (if any) placed close to the **receiver**. RV1103 has on-die 80–125 Ω
differential termination (Table 3-3), so no external termination is needed.

**LVDS alternative:** the same SoC pins are `LVDS_RX_D0..D3`/`CK0`/`CK1`, the sensors support
low-voltage LVDS via `OMODE` high, and `imx327.c` has a full `V4L2_MBUS_CCP2` mode path. It is a real
fallback, but CSI-2 is the better-trodden route.

### 5.2 INCK — no external oscillator needed

Both drivers require **exactly 37.125 MHz** (`imx327.c:49`, and `imx290.c:1013` rejects anything else).

RV1103 can generate it exactly from the SoC:
- `GPLL` is set to **1 188 MHz** in `rv1103.dtsi` (`assigned-clock-rates`).
- `CLK_REF_MIPI0_SRC` is a GPLL/CPLL mux with a **5-bit divider** (`clk-rv1106.c:402`), so ÷1…32.
- **1188 / 32 = 37.125 MHz exactly.** (74.25 MHz = 1188/16, also exact.)
- It comes out on `MCLK_REF_MIPI0` → **pin 1, `MIPI_CLK0_OUT`**, pinmux `mipi_refclk_out0`.

This is the same path the SC3336 already uses. **Route pin 1 → sensor P3 `INCK`** and skip the crystal.

Signal integrity: 37.125 MHz on a short trace is undemanding, but it is a clock next to the CSI pairs —
keep it referenced to ground and away from the differential pairs, and leave a footprint for a series
damping resistor near the SoC.

### 5.3 I²C, reset

Mirroring what the SC3336 already does on the Luckfox (`rv1103-luckfox-pico-ipc.dtsi`):

| Function | SoC pin | Note |
|---|---|---|
| SCL | 4 (`I2C4_SCL_M2`) | 400 kHz |
| SDA | 5 (`I2C4_SDA_M2`) | |
| Reset / power-down | 2 (`GPIO3_C5`) | drives `XCLR` (K10) |
| MCLK | 1 (`MIPI_CLK0_OUT`) | → `INCK` (P3) |

Pull-ups: Sony says 1 kΩ to OVDD. With one sensor on a short bus, 2.2 kΩ is also defensible; fit 1 kΩ
per the datasheet and leave the option to depopulate.

### 5.4 The IO-voltage problem — read this before laying anything out

**The sensor's OVDD is 1.8 V.** Its digital inputs are specified `VIH ≥ 0.8 × OVDD` and
`VI ≤ OVDD + 0.3 V`. **A 3.3 V SoC IO driving `INCK`, `XCLR`, `SCL`, `SDA`, `XVS` or `XHS` exceeds the
sensor's absolute input rating.**

So the RV1103 IO bank that carries pins 1, 2, 4, 5 (and whatever you pick for XVS/XHS) **must be set to
1.8 V**. If it must be 3.3 V for another reason, you need level shifters on six-plus signals including a
37 MHz clock — ugly, and a good reason to avoid it.

**RESOLVED (2026-10-14) — no level shifters needed.** Luckfox's Pico Plus/Mini schematics label the
bank powered by pin 88 as **"VCCIO7 MIPI_CSI/LVDS_RX/UART/I2C (1.8V only, some for input only)"**, and
it contains pins **1–5** (MIPI_CLK0_OUT, GPIO3_C5, GPIO3_C6/PWM7, I2C4 SCL/SDA) as well as the MIPI
pins 76–87. So the whole sensor interface is natively 1.8 V. "Some for input only" refers to the CK1
pins 78/79, which Luckfox labels `GPI3_B2`/`GPI3_B3` (input-only). The other bank labels on the same
schematic: VCCIO1 (UART2/JTAG, pins 42–51) 3.3 V only; VCCIO2 (SARADC, 15–16) 1.8 V only; VCCIO3
(FSPI) 1.8/3.3 via 0 Ω option; VCCIO4 (SDMMC, 8–14) labelled 3.3 V only; VCCIO6 (GPIO1_C/D, 52–65)
1.8/3.3 via 0 Ω option.

The original question, kept for context: the brief datasheet does not say which `VCCIO` supplies the GPIO3_C/D
bank. Candidates are `VCCIO4_VCC3V3` (pin 7) and `GPIO3_VCC` (pin 27); both appear to be 1.8/3.3 V
selectable. What you need from the TRM (GRF chapter) or the reference schematic:

1. Which VCCIO pin powers `GPIO3_C4/C5/C6/C7/D0` (pins 1–5)?
2. Does that same bank also power the SDMMC pins 8–14?
3. Is the selection a strap, a GRF register write, or purely "whatever you supply the pin with"?

If the SDMMC bank shares the supply and the board boots from SPI NAND instead (see §8), the conflict
disappears and the whole bank can sit at 1.8 V natively.

---

## 6. Exposure: what the register widths actually allow

1H (line period) is `HMAX / 74.25 MHz` — the horizontal counter runs at **2× INCK**. Confirmed against
the datasheet's own table: `HMAX = 0x898 = 2200` → 29.6 µs, × `VMAX = 0x465 = 1125` lines = 33.3 ms
= 1080p30. ✅

```
Integration time = frame period − (SHS1 + 1) × 1H
frame period     = VMAX × 1H          (master mode)
                 = XVS interval       (slave mode)
```

Register widths: `VMAX[17:0]` (max 262 143), `HMAX[13:0]` (max 16 383), `SHS1[17:0]`.

This is a **much better position than the SC3336**, whose 16-bit VTS was the ceiling documented in
`CLAUDE.md` (0.899 s stock, 1.799 s after raising `SC3336_VTS_MAX`):

| Mode | 1H | Max frame period |
|---|---|---|
| Master, `HMAX = 2200` (default) | 29.6 µs | **≈ 7.8 s** |
| Master, `HMAX` stretched toward max | up to ~220 µs | **≈ 58 s** (arithmetic from register widths — *not* a Sony-stated supported range; needs validation like the SC3336 HTS experiment) |
| Slave mode | set by external XHS | unbounded — set by the XVS interval |

> The `CLAUDE.md` note that slave mode "has no register-width ceiling" is confirmed by the datasheet:
> *"When the sensor is operating in slave mode, the number of lines per frame is determined by the XVS
> interval (number of lines), using the input XHS interval as the line unit."*
>
> But **master mode alone already gives ~7.8 s at the default HMAX**, which may be enough to make slave
> mode unnecessary. Treat slave mode as an option to keep open in hardware, not a requirement.

> **Added 2026-10-14 — what Sony actually guarantees.** Both datasheets ("Long Exposure Operation",
> IMX327 p.79, IMX290 p.84): *"the maximum of long time exposure is approximately 1 s. When set to a
> number of V lines or more than that …, the imaging characteristics are not guaranteed."* This applies
> to **both** modes. So 7.8 s (master, default HMAX) is a register limit, not a supported one, and
> everything past ~1 s — in either mode — is outside Sony's guarantee and has to be validated by
> measurement, exactly as the SC3336 was. Commercial astro cameras sell IMX290/462 bodies with
> exposures of many minutes, so it evidently works; how they drive the sensor is not known here.
>
> **HMAX width is contradictory:** the register maps say `HMAX [15:0]`, the "Slave Mode and Master
> Mode" text in both datasheets says `HMAX [13:0]`. Master-mode ceiling at VMAX max (262 143):
> 14-bit HMAX → 16 383 / 74.25 MHz = 220.6 µs/line → **~58 s**; 16-bit → 882.6 µs/line → **~231 s**.
> Untested. Slave mode removes the question (frame length = XVS interval).
>
> **Decision 2026-10-14: slave mode is routed in v1** — see §7.

---

## 7. XVS / XHS — route them, in both directions

These two pins are the highest-value discretionary connection on the board, for two independent reasons.

**As inputs (slave mode, `XMASTER` high):** the SoC must supply *both* a vertical sync on `XVS` **and**
a continuous horizontal sync on `XHS` at the 1H rate — ~33.75 kHz at the default 1080p30 timing. That is
trivial for an RV1103 PWM. Note the vendor `imx327.c` runs master-mode only (it writes `XMSTA` 0x3002
and has no `XMASTER`/`XVS`/`XHS` handling), so slave mode is a driver project, not a wiring change.

**As outputs (master mode, `XMASTER` low — the default):** `XVS` pulses at frame start, at
`VOH ≥ OVDD − 0.4 V`. Wired to a SoC GPIO interrupt, **that is a hardware frame-start signal** — which
is precisely the ground truth the stale-frame bug documented in `CLAUDE.md` had to be reconstructed
from `fs_timestamp` heuristics plus a rolling-shutter discard. On this board you can know exactly when
a frame began instead of inferring it, and that bug is described as one that *follows us to the IMX*
and costs a whole multi-second exposure when it fires.

So: bring `XVS` (H11), `XHS` (L10) and `XMASTER` (F11) to SoC GPIOs, preferring PWM-capable pins. Cost
is three traces in the same corner; the payoff is optional slave mode *and* hardware frame sync.

> **Correction (2026-10-14) — the 1.8 V pin budget is tight.** Every sensor input must stay ≤ OVDD +
> 0.3 V, so these pins must come from a **1.8 V bank**. VCCIO7 (§5.4) has only five general pins —
> 1 MCLK, 2 XCLR, 3 PWM7, 4/5 I²C4 — all taken except pin 3. The candidates below on pins 8–14 are
> **VCCIO4, labelled 3.3 V only** by Luckfox, so they **must not** drive sensor pins. Plan for v1:
>
> **Decision (2026-10-14): slave mode is in v1** (the user wants the board usable as a full astro
> camera, not only a guider). So **VCCIO6 (pins 52–65) runs at 1.8 V** — a 0 Ω option Luckfox's own
> design already provides — and `XVS`, `XHS` and `XMASTER` all come from that bank:
> - `XHS` → a VCCIO6 PWM (continuous, ~33.75 kHz at default timing).
> - `XVS` → a VCCIO6 pin that is PWM-capable *and* readable as a GPIO: an output (frame period) in
>   slave mode, an input (hardware frame-start interrupt) in master mode.
> - `XMASTER` → a VCCIO6 GPIO with a **pull-down** (master by default). Sony: it must be stable before
>   `XCLR` is released and must not change during operation — a GPIO set before reset satisfies that.
> - Slave-mode input spec (IMX327 datasheet p.17): rise/fall **≤ 5 ns**, so **no resistor-divider level
>   shifting** — direct 1.8 V drive only. XVS↔XHS edges must be ≥ 1 INCK (27 ns) apart; two free-running
>   PWMs will occasionally violate that. Expected effect ±1 H (~30 µs) of frame length, **unverified** —
>   a firmware question for later.
> - Consequence: VCCIO6's pins (SDIO_M1, UART4/5_M1, I2C3_M1, SPI0) become 1.8 V. 3.3 V needs (dew
>   heater, ST-4 reserve, LX200 UART) move to VCCIO4 (pins 8–14, 7 GPIOs, UART5_M0 on 13/14).
> - Pin 3 (`PWM7_IR_M2`) and the CK1 pads become spares.

Good candidates (all on the left edge, same bank as the rest of the camera interface):
- pin 3 — `PWM7_IR_M2 / MIPI_CLK1_OUT / GPIO3_C6` — PWM-capable, adjacent, and `MIPI_CLK1_OUT` is unused
  in single-sensor 4-lane mode. Natural choice for `XHS`.
- pins 8–14 — free if the board boots from SPI NAND (§8), and `GPIO3_A2/A3/A4/A5` carry
  `PWM9_M0`/`PWM8_M0`/`PWM10_M0`/`PWM11_IR_M0`.

---

## 8. Boot medium, and the pins it frees

The BootROM boots from **SPI (FSPI)** or **SD/MMC**, and accepts code download over **USB or UART**.
The roadmap in `CLAUDE.md` targets SPI NAND. Doing that has a second benefit beyond flash size:

**The seven SDMMC pins (8–14) become free GPIO3_A1..A7 on the same left edge as the camera interface,
with four PWM channels among them** (`PWM8_M0`, `PWM9_M0`, `PWM10_M0`, `PWM11_IR_M0`). That covers
`XVS`, `XHS`, `XMASTER`, `OMODE`, and leaves spares.

> **Correction (2026-10-14):** these pins are VCCIO4, which Luckfox labels **3.3 V only** — fine for
> the dew heater, sensor-LDO enables and the exposed header, **not** for anything wired to the sensor
> (see the correction in §7).

One of those spares should be the **dew heater**. `CLAUDE.md` records that on the Luckfox the heater
had to be plain on/off because `/sys/class/pwm` was not exported and there was no `pwm` DTS node. On a
custom board, putting the heater FET on a PWM-capable pin plus a `pwm` node and `CONFIG_PWM_SYSFS`
turns it into a proportional control for free. The existing `OAG_DEWHEATER_GPIO` mechanism already
degrades gracefully, so this is an upgrade, not a rewrite.

Also worth remembering from `CLAUDE.md`: the current Luckfox board has **no populated internal flash**,
which is why USB flashing does not work and every image update means pulling the SD card. **A populated
SPI NAND restores `upgrade_tool` / maskrom USB flashing** — a real quality-of-life win for a sealed
astronomy device with no accessible card slot.

---

## 9. USB

- **USB 2.0 high-speed only.** `rv1106.dtsi` has a DWC3 core but `maximum-speed = "high-speed"` and only
  a `u2phy`; there is no SS PHY node and no SoC pins for one. No PCB change can add USB 3.
- Pins: `USB_DM` (18), `USB_DP` (19), `USB_AVDD3V3` (20), `SARADC_USB_AVDD1V8` (17).
- **There is no VBUS or ID pin in the package.** The Luckfox DTS uses `extcon = <&u2phy>` (PHY-internal
  detection). For a peripheral-only, bus-powered device this is a non-issue — set `dr_mode = "peripheral"`
  as the current design already does.
- The board is bus-powered. Budget the SoC, DDR, sensor rails and dew heater inside the USB power
  envelope, and remember the Luckfox lesson that an unplug is an **unclean power cut**, not a shutdown —
  which interacts directly with the sensor's reverse-order power-down requirement in §2.5.
- RNDIS measured 22.5 MB/s against ~40–45 MB/s realistic for USB 2.0 bulk; `CLAUDE.md` flags CDC-NCM as
  a possible ~2× win. That is a firmware question, not a PCB one.

---

## 10. Open questions, in priority order

1. ~~Which `VCCIO` powers GPIO3_C/D (pins 1–5)?~~ **Resolved 2026-10-14:** VCCIO7, 1.8 V only (§5.4).
2. ~~Reference values for `DRAM_ZQ`, `FEPHY_EXTR`, crystal, TVSS~~ **Resolved from the Luckfox
   schematics** (§12). Still open: whether the unused FEPHY/CODEC analog supplies may be left
   unpowered — Luckfox powers them; we do the same unless proven otherwise.
3. **[OPEN] Sensor current consumption** — `TBD` in both datasheets. Needed for the USB power budget
   and for the 2.9 V/1.8 V/1.2 V regulator sizing. Measure a module or ask Sony/FAE.
4. **[OPEN] Sequencing part selection** for the three sensor rails, including a clean reverse-order
   power-down on USB unplug (§2.5).
5. ~~RV1106G3 pinout vs RV1103~~ **Resolved 2026-10-14:** different package (QFN128), not compatible
   (§4). The board uses RV1103G1 (§0).
6. `IMX290LLR` price, availability and lead time (there is no IMX327 mono — §1), and confirmation that
   its package drawing matches the colour one used here.
7. Decide 2-lane vs 4-lane. Recommendation: route 4 + CK0 (§5.1).

---

## 11. Quick decision summary

> Superseded where it disagrees by §0 (2026-10-14): RV1103G1 not RV1106, any of three sensors,
> sensor IO resolved as 1.8 V natively, XVS/XHS/XMASTER per the §7 correction.

| Decision | Recommendation | Why |
|---|---|---|
| Sensor | **IMX327** (mono LLR) | Rockchip vendor driver with `rkmodule` integration already in this tree; IMX290's frame-rate edge is irrelevant for guiding |
| Footprint | Compatible with both | Leave C1/C2/D1/D2/K1/K2/L1/L2 floating (§1) |
| Interface | CSI-2, 4 data lanes + CK0 | Pins are dedicated; costs nothing; keeps headroom. LVDS is a real fallback |
| INCK | From SoC pin 1, 37.125 MHz | Exact from GPLL 1188/32 — no oscillator (§5.2) |
| Sensor IO rail | 1.8 V, SoC bank set to 1.8 V | Sensor absolute max is OVDD+0.3 V; avoids six level shifters (§5.4) |
| Placement | Sensor at the SoC pin-1/pin-88 corner | MIPI, MCLK and I²C are all in that corner (§4) |
| XVS/XHS/XMASTER | Route all three to GPIO/PWM | Optional slave mode *and* hardware frame-start interrupt (§7) |
| Boot | SPI NAND on FSPI (28–33) | Frees pins 8–14 with 4 PWMs; restores USB/maskrom flashing (§8) |
| Dew heater | PWM-capable pin | Turns on/off into proportional control (§8) |

---

## 12. Reference design: Luckfox Pico Mini (2026-10-14)

`reference/Luckfox-Pico-Mini.pdf` is RV1103G1 + SPI NAND on one page, and is leaner than the Pico Plus
(one triple-buck PMIC where the Plus uses three separate regulator chips). **Port from it rather than
designing these circuits fresh** — the same rule as the PHD2 port. But carry over only what this board
needs; the lists below are the filter.

### 12.1 Port (values read off the schematic)

| Block | Luckfox Mini circuit |
|---|---|
| **SoC power** | **EA3036C** triple buck from `VCC5V0_SYS`. CH1 → `VCC_3V3` (1.5 µH 2520, FB 40.2 k / 9.1 k, 220 pF feed-forward); CH2 → `VDD_0V9`, max 1.6 A (1.5 µH, FB 47 k / 100 k); CH3 → `VCC_DDR` 1.8 V, max 0.4 A (1.5 µH, FB 200 k / 100 k). FB reference 0.6 V. Input 3 × 10 µF + 1 µF. |
| **Sequencing** | RC on the enables from 5 V: EN2 (0V9) 10 k, no cap → first; EN3 (DDR) 200 k→100 k + 100 nF; EN1 (3V3) 200 k + 100 nF. Order **0.9 → 1.8 → 3.3** (the Plus labels the same as Step 1/2/3). |
| **1.8 V** | `VCC_1V8` = `VCC_DDR` through a ferrite bead (DDR2 is 1.8 V, so one buck serves both). |
| **CPU rail** | `CPU_DVDD` (74) tied to `VDD_0V9` — no separate DVFS regulator. |
| **Crystal** | 24 MHz, 2016 4-pad, 2 × 18 pF C0G, 1 MΩ across, 22 Ω series on XOUT. |
| **Passives** | `DRAM_ZQ` (37) 240 Ω 1 % to GND; `FEPHY_REXT` (72) 6.04 kΩ 1 % to GND. |
| **USB-C** | 5.1 kΩ on CC1 and CC2 (sink), VBUS straight to `VCC5V0_SYS` (the Plus adds an MBR230 Schottky), 10 µF + 100 nF. D+/D− direct to pins 19/18. |
| **SPI NAND** | **W25N01KVZEIR** (WSON-8), 10 k pull-ups on D2 (WP#) and D3 (HOLD#). |
| **Recovery** | `SARADC_IN0`: 10 k pull-up to 1.8 V, 100 Ω series, 1 nF C0G to GND. Luckfox's note: **"SARADC_IN0 must always be pulled-up"** — it is the BOOT/recovery key. U-Boot reads it as `adc-keys` channel 0 (`rv1106-luckfox.dts`), and Luckfox documents that holding BOOT at power-up gives true MaskRom (USB ID 2207:110c). |
| **Debug UART** | `UART2_M1`, pins 49 (TX) / 50 (RX) — the SDK's `fiq-debugger` console. |

### 12.2 Drop

Micro-SD socket and its parts; Ethernet magnetics, connector and series resistors (keep `FEPHY_REXT`
and the FEPHY/OTP AVDD supplies + decoupling — Luckfox powers them, and we do not know that leaving
them unpowered is safe); CODEC mic/line-out parts (keep `CODEC_AVDD1V8` powered); both LEDs; the key
(replaced by pads); the 2×20 pin header; the 20-pin FFC camera connector; the VBUS divider to
`USB_DET_IN` (RV1103 has no VBUS-detect pin; Luckfox only routes it to a test point); the 0 Ω
VCCIO voltage-select options (fix each bank at its one voltage: VCCIO_FLASH 3.3 V, **VCCIO6 1.8 V**
for slave-mode XVS/XHS/XMASTER — §7).

On keeping the unused FEPHY/CODEC supplies: no Rockchip statement either way could be found. Pin 66
(`OTP/ETH/TSADC_AVDD1V8`) is not optional at all — the same pin feeds the OTP (chip ID/calibration)
and the thermal sensor. Pins 71 (`FEPHY_AVDD3V3`) and 23 (`CODEC_AVDD1V8`) are separate and *might*
be leaveable unpowered, but an unpowered analog domain next to a powered digital one can leak or
back-power through protection diodes, and every known-working RV1103 design (Pico, Plus, Mini — the
Mini has no Ethernet jack and no audio parts, and still powers both) connects them. Cost of
connecting: a trace and ~2 caps. Cost of being wrong: a respin. Keep them.

### 12.3 Add (not on the Luckfox)

- **Sensor rails:** DVDD 1.2 V, OVDD 1.8 V, AVDD 2.9 V — three small LDOs from `VCC_3V3`, sequenced
  1.2 → 1.8 → 2.9 (§2.5). OVDD gets **its own** LDO rather than sharing `VCC_1V8`, so the sensor can
  be powered down and sequenced independently of the SoC. Consequently the I²C pull-ups go to sensor
  OVDD, and the driver must keep MCLK off until the sensor rails are up (the Rockchip power-on
  sequence already does: regulators → MCLK → reset).
- **Sensor decoupling:** 24 caps + 4 reference caps (§2.2/§2.3), all on the front with the sensor.
- **NTC** + 1 resistor on `SARADC_IN1` (pin 15, 1.8 V bank), next to the sensor.
- **Dew heater:** JST-XH 2-pin, low-side N-FET, gate pull-down.
- **USB ESD** protection on D+/D− (cheap; the Luckfox has none, but this device is plugged into
  field laptops).
- **Exposed header** with spare 3.3 V GPIOs (reserve 4 for a later ST-4 adapter) and a UART.

### 12.4 Recovery and debug pads

| Pads | Purpose |
|---|---|
| `RECOVERY` + `GND` | Short while plugging in → MaskRom / download mode. Covers a broken U-Boot as well as a broken kernel/rootfs. |
| `UART2_TX`, `UART2_RX`, `GND` | Serial console (U-Boot + kernel). The only way to see why a fresh board does not boot. |

`reboot loader` from Linux remains the first-line path; the pads are for when Linux does not come up.
For production they may be depopulated if space is needed (the SARADC_IN0 **pull-up stays**).

---

## 13. Schematic v1 (2026-10-14)

Drawn in EasyEDA from `schematic/design.py` (see `schematic/README.md`); that file is the authoritative
pin/net list with reasoning per block. Verified: EasyEDA netlist matches the spec pin for pin, strict
DRC clean. **Not yet reviewed by the user.** Decisions taken while drawing:

- **SoC pin plan.** VCCIO7 (1.8 V): 1 MCLK (22 Ω series), 2 XCLR, 4/5 I²C4, 3 spare. VCCIO4 (3.3 V):
  8/9/10/12 ST-4 reserve, **11 DEW_EN** (the only pin of the bank with a default *pull-down*, so the
  heater stays off through reset), 13/14 UART5_M0 for LX200. VCCIO6 (1.8 V): 52/53/54 sensor rail
  enables, 55 XVS (PWM0_M1), 56 XMASTER, 57 XHS (PWM3_IR_M2) — all default pull-down: rails off,
  master mode. 60–65 (SDIO_M1) left free for Wi-Fi. SARADC IN0 recovery, IN1 NTC. 48 and 42 unused.
- **Sensor rails.** Three LDOs with separate enables; ordering (on DVDD→OVDD→AVDD, off reversed) is
  to be done in the device tree by chaining `vin-supply`, because the vendor `imx327.c` enables all
  three together via `regulator_bulk_enable` — and also starts MCLK *before* the regulators, which
  violates Sony's sequence and needs a driver fix. AVDD 2.9 V and OVDD 1.8 V: **LP5907** (6.5 µVrms)
  from 3.3 V. **DVDD 1.2 V: TLV75512P from the 1.8 V rail** — DVDD carries most of the sensor current
  (Sony: TBD; ~100 mA assumed), and from 3.3 V a linear LDO would dissipate ~0.2 W right next to the
  sensor. The EA3036 channel behind the 1.8 V rail is rated 2 A (EA3036 datasheet; Luckfox's "0.4 A"
  was a load annotation). LP5907 and TLV755P share the SOT-23-5 pinout. EN thresholds checked against
  both datasheets for 1.8 V GPIO drive: LP5907 VIH 1.2 V, TLV755P 1.0 V. Both have active output
  discharge (230 Ω / 120 Ω).
- **SoC power** ported from the Luckfox Mini: EA3036C, 1.5 µH 2520 inductors, RC-sequenced enables.
  3.3 V feedback changed to basic-part 100k/22k (3.33 V). VCC_DDR→VCC_1V8 through a 0 Ω footprint
  (Luckfox's "0R" ferrite position) so a bead can be fitted later.
- **Crystal** HELE X2B024000BC1HA-V (2520, CL 12 pF) with Luckfox's 18 pF / 1 M / 22 Ω.
- **USB:** USBLC6-2SC6 added (not on the Luckfox).
- **Recovery / debug / I/O** as unassembled 2.54 mm header footprints (J201 recovery+GND, J202
  UART2 TX/RX/GND, J203 1×8: 3V3, ST-4 ×4, LX200 TX/RX, GND). Could shrink to bare pads later.
- **Mechanical parts with through-hole legs:** USB-C (shell pegs) and the JST-XH (only THT B2B-XH-A
  is in the library) — both on the back side; affects assembly cost.
- Unused but deliberately powered: FEPHY AVDD (71) + EXTR 6.04 kΩ, CODEC AVDD (23). CODEC_VCM (22)
  left open (codec never enabled).

---

## 14. Placement v1 (2026-10-14)

Generated by `layout/place.py` (ICs hand-placed, passives packed next to the pin each one serves);
**awaiting user review, not routed.** Board 40 × 40 mm centred on (0,0); M2.5 holes at (±15, ±15),
3.5 mm keep-out radius.

- **Sides (changed on user request): the sensor alone is on the PCB *bottom* layer** — that side is
  the camera front, facing the lens — **and every other part is on top**, which keeps the EDA view
  readable and is the usual one-busy-side assembly. Nothing but the sensor (and the through-hole
  header's pads) appears on the lens side.
- **Sensor optical centre exactly at (0,0).** From Sony's "Optical Center" drawing (IMX327 p.7, top
  view, A1 top-right) the optical centre is 0.058 mm toward the P column and 0.235 mm toward row 1 from
  the package centre, i.e. (+0.058, −0.235) mm in the library footprint's coordinates. On the bottom
  layer EasyEDA flips the footprint in y, so that becomes (+0.058, +0.235) and the footprint origin is
  placed at (−0.058, −0.235) mm. Verified from pad positions after the move.
- **Top side:** SoC at (11.5, 1), rotation 90 — MIPI pins 76–87 along its left edge facing the
  sensor's N/P columns (3–4 mm), MCLK/I²C (1–5) at its bottom-left corner next to the sensor's control
  pins (rows 10/11, now the sensor's lower edge). As far from the sensor as 40 mm allows with short
  MIPI. NAND below the SoC, crystal at its OSC corner (top right). Sensor decoupling directly over the
  sensor (each cap at its own supply pin's via). Sensor LDOs, I²C pull-ups and strapping above the
  sensor. USB-C (vertical) + ESD + buck on the left. Dev pads bottom left. XH dew-heater connector
  bottom centre, opening facing up; the plug zone above it (x ±3.6, y −10…−2.2 mm) is kept free. 1×8
  I/O header along the top edge.
- **EasyEDA facts learned:** PCB y points up. On the back layer footprints are flipped in **y**, so
  rotations behave differently there (the XH needs rotation 0 on the back to open upward).
  Component pad IDs are the component's primitive ID + a suffix; `pcb_PrimitivePad.getAll()`
  returns all pads, grouped by that prefix. `pcb_Primitive.getPrimitivesBBox` gives full extents.

---

## 15. HANDOFF — where the PCB stands (end of session 2026-10-14)

**State in EasyEDA (project `OAG_IMX327`, all saved):** schematic final for now (3 A3 pages, verified:
`schematic/check.py` 0 mismatches, strict DRC 0). PCB1: 4 copper layers, 40 × 40 mm outline centred on
(0,0), four M2.5 NPTH holes at (±15, ±15), all 144 footprints imported and given a **first-pass
placement** by `layout/place.py` (sensor alone on the bottom layer, everything else on top). **Nothing
is routed.**

**User decisions taken after placement v1 (not yet implemented):**
1. **Area behind the sensor (top side, inside its outline) = thermal contact pad.** No components
   there. An exposed copper area (soldermask opening) on GND with thermal vias, for a cold finger /
   heatsink / future TEC. The sensor's inner-pad escape vias still go there — tented or filled.
2. **Sensor decoupling and reference caps (C307–C338, 28 parts) move to the bottom (lens) side,
   directly beside the sensor package** — same layer as its pads, no via in the path. The sensor LDOs
   (U302–U304) and their caps (C301–C306), strapping and I²C pull-ups stay on top.
3. **Critical blocks get hand layout, not the packer.** The user rightly objected that the packer
   (nearest-free-spot to the served pin) knows no layout rules. Re-do by hand:
   - **Buck (U102 EA3036C)** — per its datasheet (`reference/EA3036 datasheet V1.1.pdf`, p.11): input
     caps (10 µF, C108–C110 on VIN1/2/3, C111 1 µF on VCC) as close as possible to each VINx/GNDx
     pair; inductors L101/L102/L103 straight off LX1(11)/LX2(6)/LX3(20) with short wide LX copper on
     top; output caps right at the inductor outputs, their grounds stitched to the inner GND plane
     with their own vias; feedback dividers (R103/R104/C103 → FB1 pin 14, R105/R106/C104 → FB2 pin 3,
     R107/R108/C105 → FB3 pin 2) next to the FB pins and routed **away from the LX nodes**, sensing at
     the output caps; AGND (15, 17) to the GND plane by its own via; exposed pad to GND with several
     thermal vias; VIN on an inner layer. The datasheet also suggests 4 layers with LX/output on top.
     Note EN pull-ups (R109–R111) and timing caps (C106, C107) are not layout-critical.
   - **Crystal Y201** — load caps C201/C202 at the crystal pads, short symmetric traces to SoC pins
     45/46, R202 (22 Ω) at XOUT, R203 (1 M) across; no other signals under it; GND guard.
   - **USB** — J101 → U101 (ESD, in line) → SoC pins 18/19 as a 90 Ω differential pair; CC resistors
     R101/R102 at the connector; VBUS caps C101/C102 at the connector.
   - **SoC decoupling** — 100 nF caps at their supply pins (the packer's result is a start).
4. **Still unanswered by the user** (asked, no reply yet): (a) SoC heat — its edge is only ~0.6 mm
   outside the sensor's outline (opposite side) to keep MIPI 3–4 mm; OK or trade MIPI length for
   distance? (b) dew-heater plug — the XH (HCTL XH-2AW, side entry) sits bottom centre with its opening
   facing up and a free zone of ~7 × 8 mm for the plug; a top-entry XH would free that zone if the
   cable can leave rearward. (c) USB-C top-left and the I/O header along the top edge vs the housing.

**Suggested order for next session:** start the bridge (user runs the command in CLAUDE.md) → read this
section → implement decisions 1–2 in `layout/place.py` (sensor caps to the bottom ring, keep the
sensor shadow clear on top) → hand-place the buck, crystal and USB blocks (write them as explicit
coordinates in `place.py`, packer only for the rest) → show the user → define the stack-up and rules
(L1 signal, L2 solid GND, L3 power, L4 = bottom/sensor side; 90 Ω USB pair, 100 Ω MIPI pairs, JLC
4-layer impedance stack) → route MIPI first, then power, then the rest.

**Process gotchas:**
- Schematic → PCB import needs the **user** to confirm a dialog in EasyEDA (`pcb_Document.importChanges()`
  only opens it). Every part now has a stable unique ID `gge<designator>`, intended to let a re-import
  keep placements — **not yet verified**; check on the first post-placement schematic change, and
  prefer changing single parts over regenerating whole pages once routing exists.
- `generate.py` redraws whole pages (it clears them first). `check.py` must report 0 mismatches after
  any change.
- EasyEDA PCB quirks: y up; bottom-layer footprints are flipped in **y**; component pad IDs = component
  primitive ID + suffix; `pcb_PrimitivePad.getAll()` returns all pads; `pcb_Primitive.getPrimitivesBBox`
  gives full extents. Enums are not globals in the bridge: use literal values (TOP 1, BOTTOM 2,
  BOARD_OUTLINE 11, MULTI 12, INNER_1 15, INNER_2 16).
- Screenshots: `getCurrentRenderedAreaImage` after `zoomToRegion(left, right, top, bottom)`; helper
  scripts were in the session scratchpad (gone) — trivial to rewrite (see `schematic/README.md`).

**Nothing from this session is committed to git** (`PCB/` was untracked at session start; the user did
not ask for a commit).

---

## 16. Requirements revision: two use cases on one board (2026-10-15)

Decided with the user on 2026-10-15, before any routing. Summary rows are in §0 (marked
**rev 10-15**). **Not yet in the schematic or placement** — `schematic/design.py` and
`layout/place.py` still reflect §13/§14 and the 40 × 40 outline.

### 16.1 Why one PCB

The OAG and Zenit variants share SoC, flash, sensor, sensor rails, buck, USB-C and Wi-Fi. They differ
only in small parts: dew-heater FET + XH (OAG), LX200 header (OAG), battery connector (Zenit), NTC
(optional on Zenit). One layout means one bring-up, one device tree with small overlays, one test
procedure; at 50–100 boards per order a second layout costs far more than a few DNP footprints. Where
the variants need different extras, they may share a SoC pin (one net, two footprints, one populated) —
e.g. SARADC_IN1 = NTC on OAG, BAT_SENSE divider on Zenit (both SARADC channels are otherwise taken:
IN0 recovery, IN1 NTC).

### 16.2 Power: USB-C is the only input

- **No OR-ing.** Diodes were rejected (a Schottky drops ~0.35–0.45 V at any current: ~8 % of all
  power lost as heat, ~15 % less dew-heater power since P ∝ V²); a TPS2116 mux (~$0.56, LCSC C3235557,
  late-2023 price) was rejected on cost. Both became unnecessary once the mount's 5 V arrives through the
  same USB-C as a PC (§16.3): there is only one plug, so only one source.
- **Mount power:** OAE supplies a clean 5 V / 2 A on its XH-4. No 12 V input on the camera — the dew
  heater must run at 5 V.
- **Battery board (Zenit), external, not designed yet.** It feeds the same 5 V node through a board
  connector, so it — not the main board — must provide: reverse-current blocking (ideal diode), and
  switching its output off whenever a USB source is attached (otherwise it back-feeds a PC's port and
  fights its 5 V). The main board gives it what it needs to know that:

  | Pin | Direction | Purpose |
  |---|---|---|
  | 5 V, GND | battery → main | system power |
  | CC1, CC2 | main → battery | carry a voltage only when a USB source (PC, charger, mount cable) is plugged in, independent of VBUS |
  | POWER_HOLD | main → battery | button starts the battery board's boost; SoC holds this high; SoC releases it only after a clean shutdown (an unplug is an unclean power cut — see the zeroed-binary incident in `CLAUDE.md`) |
  | BAT_SENSE | battery → main | divided battery voltage into SARADC_IN1 (Zenit variant) |
  | BUTTON (optional) | battery → main | button state, e.g. long press = re-pair Wi-Fi |

- Runtime target 8 h minimum, 12 h+ preferred. Rough estimate only (not measured): ~1–1.5 W average
  with Wi-Fi → 8–18 Wh, i.e. one 21700 or two 18650 cells. Measure on the Luckfox with a USB meter
  before sizing the battery.

### 16.3 Mount link: one USB-C cable carries power and data

```
N.I.N.A. ──TCP localhost:4400──► OAT ASCOM driver ──COM port (USB)──► ESP32 ──UART──► camera (PHD2 port)
           "looks like PHD2"      relays frames     existing cable    relays     custom XH-4→USB-C cable
```

- **Cable** (custom assembly): OAE XH-4 → USB-C plug. 5 V → VBUS, GND → GND, ESP32 GPIO21 (TX, open-drain)
  and GPIO22 (RX) joined into one conductor → plug SBU1 *and* SBU2. A pull-up resistor to 5 V on CC in the
  plug (as every A-to-C cable has) marks it as a source, which also drives the battery-board CC logic.
- **Why SBU1+SBU2 tied:** USB-C swaps them when the plug is flipped; one shared line makes orientation
  irrelevant. The price is half-duplex, which is harmless because the **camera is the only initiator**
  (it sends LX200 commands and polls for tunnel data; the ESP32 only answers).
- **A normal PC cable** leaves SBU unconnected — the board behaves as before. A future OAE board with a
  USB-C socket could use a stock cable, but it must be a *full-featured* one (cheap USB 2.0 C-to-C cables
  do not wire SBU); with the wrong cable power still works and the error LED reports "no mount link".
- **ESP32 side:** GPIO21/22 have no boot-strapping role (12 does — avoid; 34/35/36/39 are input-only).
  OAE does not use I²C today. The ESP32 has three hardware UARTs routable to any GPIO; no SoftwareSerial.
- **N.I.N.A. integration without Wi-Fi:** on-camera PHD2 gets a PHD2-compatible event server (JSON-RPC,
  already on the guiding to-do list). For OAE, the OAT ASCOM driver — which owns the COM port — hosts a
  stand-in server on `localhost:4400` (N.I.N.A.'s default) and relays over the mount's serial link,
  **polling** for queued camera messages (~100 ms) so OAT's request/response Meade parser never sees
  unsolicited data. Traffic is ~1 kB/s of JSON. Live images do **not** fit (the 182 kB preview would take
  ~2 s at 921600 baud). Without an OAE mount the camera is cabled to the PC and RNDIS carries :4400.
  OAT ASCOM driver / OAE firmware changes have not been scoped yet.
- **Wi-Fi role (OAG): standalone operation only** — a user with a phone and no PC (e.g. a DSLR for
  imaging) starting polar alignment / guiding. Never in the guiding, dither or N.I.N.A. path.

### 16.4 One UART, two physical links (proposed circuit)

J203 (full duplex, mounts such as ATmega2560-based OAT at **5 V logic** — unverified, see
`PlateSolving/README.md`) and the SBU link (half-duplex, ESP32 at 3.3 V) share UART5_M0 (pins 13/14,
VCCIO4, 3.3 V). Tying them directly does not work: a push-pull 5 V TX on J203 would fight our TX on the
shared SBU node and push current into a 3.3 V SoC pin. Proposed instead, all parts powered from 3.3 V
with 5.5 V-tolerant LVC inputs/outputs:

- **RX merge:** a 74LVC1G08 (AND) combines J203-RX and the SBU node into SoC RX. UART idles high, so
  whichever line pulls low passes. Its 5 V-tolerant inputs replace the 74LVC1G17 suggested by the
  plate-solving session (same protection, one gate).
- **TX to J203:** series resistor (~100–330 Ω) from SoC TX.
- **TX to SBU:** SoC TX → 74LVC1G07 (open-drain) → SBU node, with a 4.7 kΩ pull-up to 3.3 V on the node.
  Both drivers on that node (ours, ESP32's) are open-drain, so a collision cannot short anything; the
  ESP32 hears its own echo and so do we (discard in software).
- **Series resistors** (~1 kΩ) in front of the gate inputs, and **ESD** on J203-RX, J203-TX and SBU.
  The resistors also limit current if the other side is powered while we are not (and vice versa), or if
  an RS-232 port is plugged in by mistake.
- Using J203 and the SBU cable at the same time is not supported.
- **v1 population (2026-10-15): the J203 path is laid out but DNP** (other-mount serial is not a priority).
  With the 74LVC1G08 unpopulated, a 0 Ω option bridges the SBU node straight to SoC RX, so the OAE link
  works on its own. Populating the J203 path later is an assembly change, not a layout change (not field-
  retrofittable).

### 16.5 Open items from this revision

1. **Wi-Fi module selection:** 1.8 V SDIO, LPO (32.768 kHz) requirement — the RV1103 has no RTC
   clock output, and a PWM from 24 MHz gives 32.787 kHz (~600 ppm off, likely outside spec), so either a
   module with an internal LPO or a small 32 kHz oscillator. Plus one or two GPIOs (WL_REG_ON, host wake).
2. **Pin budget — checked 2026-10-15 against the datasheet pin table (Table 2-1; suffix `_u`/`_d`/`_z` =
   reset pull-up/pull-down/none).** Proposed allocation (not yet in `design.py`):

   | Signal | Pin | Why there |
   |---|---|---|
   | WL_REG_ON (Wi-Fi enable, out) | **48** GPIO1_A2_d (also CPU_AVS, unused), 3.3 V | The chosen module's CHIP_EN is in its 3.3 V domain (internal 100 kΩ pull-up; Ebyte's reference circuit adds 100 kΩ to VDD33), so it must not sit on a 1.8 V pin. Pull-down = module held off through reset (§16.8) |
   | Error LED (out) | **3** GPIO3_C6_d, VCCIO7 1.8 V, **via an NPN** (e.g. MMBT3904) switching the LED from 3.3 V | An LED cannot be driven well from 1.8 V; a 1-cent transistor fixes that. Pull-down = LED off through reset |
   | POWER_HOLD (out, Zenit) | **42** GPIO0_A4_d, `PWR_CTRL_M0`, PMU domain 3.3 V | Its intended function. **Pull-down at reset**, so the battery board must keep itself on until Linux asserts it (button held, or its own latch) |
   | BUTTON (in, Zenit) | **8** GPIO3_A1_u, shared with ST-4 RA+ | Variant sharing: ST-4 is OAG-only, the battery board Zenit-only. Pull-up suits an active-low button |
   | WL_HOST_WAKE | **none** | Use the SDIO in-band interrupt (DAT1); out-of-band wake only matters for suspend, which this device never does |

   **Confirmed:** ST-4 pins 8/9/10/12 are `_u` (pull-up) — the active-low opto rule in §16.6 holds.
   After this the OAG variant has **no spare digital pin**. Further pins could come from: XMASTER as a
   resistor strap instead of a GPIO (1 pin; loses run-time master/slave switching — **user, 2026-10-15: keep the GPIO for the dev board so slave mode can be tested from software; the strap is the fallback**), hardware-sequenced
   sensor-rail enables (2 pins; reverse-order power-down gets harder), or MIPI CK1N/P (78/79, `GPI3_B2/B3`,
   **input-only**, unused in 4-lane mode — whether they work as GPI while CSI runs on the other lanes is
   **unverified**, needs the TRM).
   **SDIO_M1 is a proven Wi-Fi bus on this exact chip:** Rockchip's own `rv1103g-battery-ipc-v10/v11.dts`
   (an RV1103G battery camera reference) runs Wi-Fi on `&sdio` with `sdmmc1m1_*` (= pins 60–65), 4-bit,
   30 MHz, `cap-sdio-irq` (in-band interrupt, no host-wake pin). Luckfox's Pico Ultra W instead puts its
   AIC8800DC on the SD-card controller (`&sdmmc`, 3.3 V VCCIO4 pins) — not an option here, those pins carry
   ST-4/dew/UART.
3. **Antenna:** chip antenna at a board edge with its copper-free zone, as far from the sensor as
   possible (Wi-Fi TX heat and noise).
4. **Battery-board connector** type and position (Zenit).
5. Re-do schematic (`design.py`) and placement (`place.py`) on the new outline; §15's placement
   decisions (thermal pad behind the sensor, sensor caps on the lens side, hand-placed buck/crystal/USB)
   still apply.

### 16.6 ST-4 as a retrofit kit on a small connector (2026-10-15)

User decision: an RJ12 jack is too large for the board, so ST-4 is exposed on a small locking connector
and the jack lives on a separate kit. Proposed split:

- **Main board:** ST-4 GPIOs 8/9/10/12, 3.3 V, GND on the connector, with series resistors and ESD
  (the lines leave the board). Cost to non-ST-4 users: the connector and a few passives.
- **Kit PCB:** RJ12 (6P6C, SBIG pinout: 2 GND/common, 3 RA+, 4 Dec+, 5 Dec−, 6 RA−) and four
  optocouplers, each shorting its line to the mount's common. Opto isolation copes with 5 V or 12 V
  pull-ups and different grounds on different mounts, as commercial guide cameras do. Retrofittable.
- **Polarity:** opto LEDs active-low (anode on 3.3 V, cathode via resistor into the GPIO). Pins 8/9/10/12
  default high at boot, so the mount sees no pulses while the camera boots or is unpowered.
- **Decided (user, 2026-10-15): two separate connectors, both unpopulated in v1.** ST-4: 5-pin JST-GH
  (4 signals + 3.3 V as the opto common; the mount-side common comes from the RJ12, so no GND). UART:
  3-pin JST-GH (TX, RX, GND — a serial link to a separately powered device needs the shared ground).
- **Why the polarity is a main-board concern even though the optos are on the kit:** it fixes what the
  connector carries (3.3 V, not GND, as the common) and relies on the main board's GPIO boot defaults.
  An active-high kit (cathodes to GND) on default-high pins would fire all four directions while the
  camera boots. **To verify:** the reset pull state of pins 8/9/10/12 in the datasheet (§13 only states
  that pin 11 is the bank's sole default pull-down).

### 16.7 Other mounts (noted 2026-10-15, not a priority)

Standalone guiding / polar alignment / basic GoTo for non-OAT mounts is software on top of hardware already
planned: LX200 family (OAT, OnStep, Meade) via SBU or J203; Sky-Watcher SynScan motor protocol via J203 into
the hand-controller socket (EQDIR-style TTL) or over Wi-Fi (AZ-GTi, SA GTi); Celestron NexStar and iOptron via
the external RS-232 adapter; ST-4-only mounts via the kit. Polar alignment needs no protocol (manual RA
rotation, SharpCap-style) and push-to via plate solving works on any mount. Implement protocols from the
vendors' published command sets — INDI/EQMOD drivers are (L)GPL, alpacad so far uses only MIT/BSD code. Real
cost is testing on real mounts and GoTo safety limits. **Only hardware dependency: J203 TX/RX must stay
5 V-tolerant TTL (§16.4).**

### 16.8 Wi-Fi module selection — findings so far (2026-10-15, in progress)

**Hard requirements** (a module failing any of these is out):
- **1.8 V SDIO IO.** SDIO_M1 (pins 60–65) sits on VCCIO6, fixed at 1.8 V by the sensor sync/enable
  signals on the same bank. No level translator (space, SDIO timing).
- **A Linux driver already in this SDK** (`sysdrv/drv_ko/wifi/`, loaded by `insmod_wifi.sh`): aic8800dc
  (driver *and* firmware blobs in-tree, 4.8 MB source dir), rtl8723ds, rtl8723bs, rtl8723cs, rtl8189fs,
  rtl8821cs, rtl8822bs, bcmdhd / cywdhd (Broadcom/Cypress/Infineon), ssv6x5x / ssv6115, atbm603x /
  atbm6441, hichannel (Hi3861). Kernel-side tree: `sysdrv/source/kernel/drivers/net/wireless/rockchip_wlan/`.
- **Sleep clock (32.768 kHz LPO) solved:** internal to the module, or a small oscillator on our board.
  The RV1103 has no RTC output, and a PWM from 24 MHz lands at 32.787 kHz (~600 ppm off).

**Preferences:** stocked at JLC/LCSC (ideally one supply, 3.3 V, + 1.8 V VDDIO from our existing rail);
small (most SDIO modules are 12 × 12 mm LGA/stamp-hole); RF pad for a chip antenna on our board
(plastic enclosure, no U.FL needed) or integrated antenna; modular FCC/CE certification; low TX peak
current (battery variant, 8–12 h). 2.4 GHz is enough; 5 GHz is a bonus for Zenit's raw frames.
Bluetooth not needed.

**What the SDK's own boards do:**
- Rockchip **`rv1103g-battery-ipc-v10/v11.dts`** (RV1103G battery camera reference): Wi-Fi on `&sdio`
  with `sdmmc1m1_*` pin groups = our pins 60–65, 4-bit, **30 MHz**, `cap-sdio-irq` (in-band interrupt),
  `non-removable`, plus an `idle` pinctrl state. This validates the bus choice on this exact chip.
- Luckfox **Pico Ultra W** (`rv1106g-luckfox-pico-ultra.dts`, `RK_ENABLE_WIFI_CHIP=AIC8800DC`):
  AIC8800DC on the *SD-card* controller (`&sdmmc`, 3.3 V VCCIO4), 4-bit, 50 MHz, `cap-sdio-irq`, reset via
  `mmc-pwrseq-simple` on a GPIO (= WL_REG_ON). Not reusable as-is (those pins carry ST-4/dew/UART here),
  but it shows the software stack works on this SDK, and that a REG_ON GPIO via `mmc-pwrseq-simple` is
  all the control the driver needs.
- **No host-wake pin is needed** by either board, so the pin budget (§16.5) stands.

**Size, from module listings seen so far:** AIC8800-family and RTL8723DS/RTL8189FTV SDIO modules from
LB-Link, Feasycom, Fn-Link, Ebyte are 12 × 12 × 1.5–1.9 mm. Several RTL8189FTV modules are listed as
3.3 V SDIO — likely disqualified by the 1.8 V rule unless their datasheet says otherwise.

**Other context:** RAM is tight (~32 MB usable today, more after the CMA trim); the Wi-Fi driver and
`wpa_supplicant` must fit beside `alpacad`. Measure on the first board. Realistic 2.4 GHz 1×1 SDIO
throughput is a few MB/s (estimate, unmeasured) — fine for previews and star lists, ~1 s per full raw
frame.

**Result (2026-10-15).** Stock and prices from JLCPCB's parts database the same day; every Wi-Fi module
there is an *extended* part.

| Module | Chip / SDK driver | SDIO IO | 32 kHz clock | Supply | Size | TX peak | JLC/LCSC | Verdict |
|---|---|---|---|---|---|---|---|---|
| **Ebyte E103-RTL8189FTV** | RTL8189FTV, 2.4 GHz 11n 1T1R / `rtl8189fs` | **1.7–1.9 V or 3.0–3.5 V** (pin 22 VDDIO) | **none** (no pin) | VDD33 3.0–3.5 V + VDDIO | 12 × 12 × 1.9 mm, 44 stamp holes | **319 mA** (11b TX 17 dBm); RX ~60 mA; idle 41 mA | C42411897, ~$1.83 @100, **187 in stock** | **Recommended for v1 (awaiting user decision)** |
| AIC8800DC module (SKI.WB800DCS2, Luckfox's part) | AIC8800DC, Wi-Fi 6 + BT / `aic8800dc` | Luckfox runs it at 3.3 V; **1.8 V unverified** | not needed (SUSCLK left open on Luckfox) | single 3.3 V | LGA-45, size unverified | unverified | **not stocked** | Fallback if its datasheet confirms 1.8 V |
| AMPAK AP6256 | BCM43456, 2.4 + 5 GHz 11ac / `bcmdhd` | 1.6–3.6 V (UHS needs 1.8 V) | **required, ±30 ppm** → add a 32.768 kHz oscillator (YXC OK252032.768KJBA4SL, C669098, $0.71) | VBAT 3.2–4.8 V + VDDIO | 12 × 12 × 1.65 mm | **1 A** | C5248079, ~$5.30 @100, 4320 stock | Only if 5 GHz is needed |
| AMPAK AP6212 | BCM43438, 2.4 GHz + BT 4.1 / `bcmdhd` | 1.7–3.6 V | required | VBAT 3.0–3.8 V + VDDIO | 12 × 12 × 1.5 mm | unverified | C2798300, ~$3.28, 1636 stock | No advantage over E103 except BT |

No stocked module was found for RTL8723DS/BS/CS, RTL8821CS, RTL8822BS, SSV, ATBM, Hi3861 or Murata 1DX.

**E103-RTL8189FTV, verified directly from Ebyte's user manual** (LCSC-hosted PDF, read page by page):
VDDIO table lists 1.7/1.8/1.9 V and 3.0/3.3/3.5 V, and pin 22's description says to choose 3.3 V or 1.8 V
to suit the host; the 1.8 V IO table gives VIH min 1.7 V, VOH min 1.6 V. Pins 1–44 contain no 32 kHz
input. Pin 12 CHIP_EN: "pulled low, turns the module off; internal 100 kΩ pull-up"; pin 13 WAKE_HOST
(not used, §16.5). Antenna is a 50 Ω stamp-hole pad (pin 2) — our chip antenna and matching network
go on our board (Ebyte's reference circuit: π-match L1/R1/C1 with an optional IPEX). SDIO 2.0, max
150 Mbit/s link rate (HT40), ±20 ppm. **Operating range −20 to +70 °C** — fine for most nights, and the
board self-heats, but it is a limit to know. Exposed centre pad is a thermal pad, grounded.

Consequences for the board:
- VDD33 from `VCC_3V3`, VDDIO (pin 22) from `VCC_1V8`, each with Ebyte's ferrite bead + decoupling.
- CHIP_EN is in the 3.3 V domain → WL_REG_ON moves to pin 48 (3.3 V) and the error LED to pin 3 via an
  NPN (§16.5 table updated). Driven by `mmc-pwrseq-simple`, as on the Ultra W.
- Up to ~0.32 A extra on the 3.3 V rail during TX: check the EA3036 CH1 budget.

**Open:** no FCC/CE certification found for the E103 (unverified either way). Fine for dev boards; for a
product sold in the EU/US an uncertified module means certifying the whole product as a radio device,
which costs far more than the module — revisit before production (the AIC8800DC or AP6256 route may win
there). Stock of 187 covers about one order: buy ahead. Real throughput still unmeasured (rough: ~1 s per
2.6 MB raw frame at 20 Mbit/s).

---

## 17. HANDOFF — end of session 2026-10-15

**Read §0 and §16 first; §15 still describes the EasyEDA state.** This session changed requirements
only — **nothing in EasyEDA was modified** (read-only calls only: 144 parts, 143 top / 1 bottom, 0 tracks,
0 vias, outline still 40 × 40). `schematic/design.py` and `layout/place.py` still encode schematic v1 /
placement v1 and are now **out of date** against §16.

**Decided this session (user):**
- One PCB, two assembly variants: OAG guider and Zenit battery remote camera (§16.1).
- Outline 50 × 50 mm, corners trimmed to Ø56.5 mm; holes stay on 30 × 30 (§0).
- USB-C is the only power input; no diode-OR, no mux. Battery is an external board, not designed now;
  main board provides its connector (§16.2).
- OAE mount link: custom XH-4 → USB-C cable, 5 V + half-duplex UART on SBU1+SBU2 tied, ESP32 GPIO21/22
  (§16.3). N.I.N.A. reaches on-camera PHD2 through the mount's USB via a stand-in server in the OAT ASCOM
  driver. Wi-Fi only for standalone (phone) use, never in the guiding/dither path.
- ST-4: not in v1; unpopulated 5-pin JST-GH (3.3 V + 4 signals); RJ12 + optos on a separate kit (§16.6).
- Reserved serial for other mounts: unpopulated 3-pin JST-GH (TX, RX, GND); its 5 V-tolerant circuit is
  laid out but DNP, with a 0 Ω bridge for the SBU link (§16.4).
- Error LED on both variants (off except blink codes).
- XMASTER stays a GPIO on the dev board (strap is the fallback for a pin).
- Other-mount protocols (Sky-Watcher, Celestron, iOptron…) parked as a future software item (§16.7).

**Recommended, awaiting the user's yes:** Wi-Fi module **Ebyte E103-RTL8189FTV** (C42411897) — the only
stocked module meeting 1.8 V SDIO + no 32 kHz clock + SDK driver; datasheet verified page by page
(§16.8). Pin plan already assumes it: WL_REG_ON on pin 48 (CHIP_EN is 3.3 V domain), error LED on pin 3
via an NPN (§16.5).

**Suggested next steps, in order:**
1. Get the user's decision on the E103 (or AIC8800DC if a module datasheet confirms 1.8 V IO).
2. Check the EA3036 CH1 (3.3 V) current budget with ~0.32 A Wi-Fi TX peaks added.
3. Pick parts: JST-GH 5-pin + 3-pin, 74LVC1G08/1G07, ESD for SBU/J203, chip antenna + π-match, NPN,
   32 kHz none needed; battery-board connector type (open).
4. Update `schematic/design.py` (new pins per §16.5, SBU wiring, battery connector, Wi-Fi block, J203
   split), regenerate, `check.py` = 0 mismatches.
5. New outline + holes in `layout/place.py`, then the §15 placement decisions (thermal pad behind the
   sensor, sensor caps on the lens side, hand-placed buck/crystal/USB) plus antenna at an edge away from
   the sensor, Wi-Fi module away from the sensor.

**Open questions for the user:** battery-board connector (type/position); whether to populate the ST-4
connector in production (deferred); E103 certification before production.

**Git:** `PCB/` committed on 2026-10-15 except the two Sony sensor datasheets (left untracked — they
are Sony documents, and this repo's remote is public; decide before adding them). `.claude/` is never
committed (machine-local permissions).
