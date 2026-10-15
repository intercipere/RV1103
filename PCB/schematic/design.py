"""OpenAstroGuider camera board -- schematic as data.

Every part, value and net of the EasyEDA schematic lives here; `generate.py`
draws it into the open EasyEDA project through the run-api-gateway bridge.
Edit this file, not the drawing, then regenerate.

Design reference: ../HARDWARE-DESIGN-NOTES.md (sections 0 and 12). Circuits
marked "Luckfox" are ported from reference/Luckfox-Pico-Mini.pdf.

Net naming: power rails VBUS / VCC_3V3 / VDD_0V9 / VCC_DDR / VCC_1V8,
sensor rails SEN_*; NC marks a deliberately unconnected pin.
"""

NC = None

# ---------------------------------------------------------------- parts
# LCSC numbers, all checked against the EasyEDA library (2026-10-14).
P = {
    # passives -- JLC basic unless noted
    "100n": "C1525", "1u": "C52923", "10u": "C15525", "4u7": "C23733",
    "10u_0603": "C19702", "10n": "C15195", "1n": "C1523", "18p": "C1549",
    "220p": "C1530", "220n": "C47129",          # 220n extended (Sony VBGR)
    "0R": "C17168", "22R": "C25092", "100R": "C25076", "1k": "C11702",
    "5k1": "C25905", "10k": "C25744", "22k": "C25768", "47k": "C25792",
    "100k": "C25741", "200k": "C25764", "1M": "C26083",
    "240R": "C25094", "6k04": "C25913",          # extended, Rockchip-specified
    # actives / mechanical (extended)
    "RV1103G1": "C5307358", "EA3036C": "C570857", "W25N01KV": "C22472329",
    "IMX290LQR": "C5249315", "TLV75512": "C2877864", "LP5907_1V8": "C92498",
    "LP5907_2V9": "C2877839", "USBLC6": "C7519", "USB_C": "C3151749",
    "AO3400A": "C20917", "XH2": "C2908611", "XTAL24M": "C254334",
    "L1u5": "C2047617", "NTC10k": "C77131",
    "TP": "C3001869", "HDR1x8": "C492407",   # TP: used for its footprint only (2.5 mm SMD pad, no hole)
}

# A page is a list of blocks. Block kinds:
#   ic     one multi-pin part; pins maps pin number -> net (NC = no-connect)
#   rows   two-pin parts drawn horizontally, pin1 left / pin2 right
#   decaps capacitors drawn vertically, net on top, GND flag below
# Coordinates are schematic units (0.01 in), A3 frame is 1655 x 1170,
# keep clear of the title block (x > 930 and y < 200).

# ================================================================ POWER
POWER = [
    # USB-C, sink only: 5.1k on each CC (Luckfox). SBU unused. Vertical
    # ("standing") receptacle so the cable plugs in from the back of the
    # guider: SHOU HAN TYPE-C 16PLT-H10.0 (10 mm tall, SMD; user's pick).
    {"kind": "ic", "ref": "J101", "part": "USB_C", "x": 260, "y": 1000, "pins": {
        "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND",
        "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
        "A5": "USB_CC1", "B5": "USB_CC2", "A6": "USB_DP", "B6": "USB_DP",
        "A7": "USB_DN", "B7": "USB_DN", "A8": NC, "B8": NC,
        "0": "GND"}},
    # USB ESD (not on the Luckfox; field laptops). Flow-through: 1/6 and 3/4.
    {"kind": "ic", "ref": "U101", "part": "USBLC6", "x": 260, "y": 760, "pins": {
        "1": "USB_DP", "6": "USB_DP", "3": "USB_DN", "4": "USB_DN",
        "2": "GND", "5": "VBUS"}},
    {"kind": "rows", "x": 520, "y": 1060, "pitch": 40, "items": [
        ("R101", "5k1", "USB_CC1", "GND"),
        ("R102", "5k1", "USB_CC2", "GND"),
    ]},
    {"kind": "decaps", "x": 520, "y": 920, "pitch": 70, "items": [
        ("C101", "10u_0603", "VBUS"), ("C102", "100n", "VBUS"),
    ]},

    # SoC power: EA3036C triple buck (Luckfox Pico Mini).
    # CH1 3.3 V (FB 100k/22k = 3.33 V; Luckfox used extended 40.2k/9.1k),
    # CH2 0.9 V (47k/100k = 0.88 V), CH3 1.8 V DDR (200k/100k = 1.80 V).
    {"kind": "ic", "ref": "U102", "part": "EA3036C", "x": 900, "y": 820, "pins": {
        "1": "GND", "2": "BUCK_FB3", "3": "BUCK_FB2", "4": "GND", "5": "GND",
        "6": "BUCK_LX2", "7": "VBUS", "8": "BUCK_EN2", "9": "BUCK_EN1",
        "10": "VBUS", "11": "BUCK_LX1", "12": "GND", "13": "GND",
        "14": "BUCK_FB1", "15": "GND", "16": "VBUS", "17": "GND",
        "18": "BUCK_EN3", "19": "VBUS", "20": "BUCK_LX3", "21": "GND"}},
    {"kind": "rows", "x": 1250, "y": 1080, "pitch": 40, "items": [
        ("L101", "L1u5", "BUCK_LX1", "VCC_3V3"),
        ("L102", "L1u5", "BUCK_LX2", "VDD_0V9"),
        ("L103", "L1u5", "BUCK_LX3", "VCC_DDR"),
        ("R103", "100k", "VCC_3V3", "BUCK_FB1"),
        ("R104", "22k", "BUCK_FB1", "GND"),
        ("C103", "220p", "VCC_3V3", "BUCK_FB1"),
        ("R105", "47k", "VDD_0V9", "BUCK_FB2"),
        ("R106", "100k", "BUCK_FB2", "GND"),
        ("C104", "220p", "VDD_0V9", "BUCK_FB2"),
        ("R107", "200k", "VCC_DDR", "BUCK_FB3"),
        ("R108", "100k", "BUCK_FB3", "GND"),
        ("C105", "220p", "VCC_DDR", "BUCK_FB3"),
    ]},
    # Start-up order 0.9 -> 1.8 -> 3.3 by RC on the enables (Luckfox):
    # EN2 10k no cap (first), EN3 100k + 100n, EN1 200k + 100n.
    {"kind": "rows", "x": 1250, "y": 580, "pitch": 40, "items": [
        ("R109", "10k", "VBUS", "BUCK_EN2"),
        ("R111", "100k", "VBUS", "BUCK_EN3"),
        ("C107", "100n", "BUCK_EN3", "GND"),
        ("R110", "200k", "VBUS", "BUCK_EN1"),
        ("C106", "100n", "BUCK_EN1", "GND"),
        # DDR and the 1.8 V IO/analog rail are the same voltage (DDR2). The
        # Luckfox joins them through a "0R" ferrite position; keep the
        # footprint so a bead can be fitted if DDR noise shows up.
        ("R112", "0R", "VCC_DDR", "VCC_1V8"),
    ]},
    {"kind": "decaps", "x": 540, "y": 420, "pitch": 55, "items": [
        ("C108", "10u_0603", "VBUS"), ("C109", "10u_0603", "VBUS"),
        ("C110", "10u_0603", "VBUS"), ("C111", "1u", "VBUS"),
        ("C112", "4u7", "VCC_3V3"), ("C113", "4u7", "VCC_3V3"),
        ("C114", "10u", "VDD_0V9"), ("C115", "10u", "VDD_0V9"),
        ("C116", "4u7", "VCC_DDR"), ("C117", "10u", "VCC_DDR"),
    ]},

    # Dew heater: 20 ohm strip at 5 V (250 mA). Low-side FET, gate held
    # low by 100k so the heater is off while the SoC boots. Side-entry XH
    # (HCTL XH-2AW, JST-XH compatible), placed inboard with its opening facing into the board so
    # neither connector nor plug passes the outline.
    {"kind": "ic", "ref": "J102", "part": "XH2", "x": 200, "y": 420, "pins": {
        "1": "VBUS", "2": "DEW_HEATER_N"}},
    {"kind": "ic", "ref": "Q101", "part": "AO3400A", "x": 220, "y": 280, "pins": {
        "3": "DEW_HEATER_N", "1": "DEW_GATE", "2": "GND"}},
    {"kind": "rows", "x": 420, "y": 300, "pitch": 40, "items": [
        ("R113", "100R", "DEW_EN", "DEW_GATE"),
        ("R114", "100k", "DEW_GATE", "GND"),
    ]},
]

# ================================================================ SOC
SOC_PINS = {
    # VCCIO7, 1.8 V only: sensor clock, reset, I2C
    "1": "SEN_INCK_SOC", "2": "SEN_XCLR", "3": NC,
    "4": "SEN_SCL", "5": "SEN_SDA",
    "6": "VDD_0V9", "7": "VCC_3V3",
    # VCCIO4, 3.3 V: ST-4 reserve, dew heater (pin 11 is pull-down by
    # default -- heater stays off through reset), LX200 UART5_M0
    "8": "ST4_RA_P", "9": "ST4_RA_N", "10": "ST4_DEC_P", "11": "DEW_EN",
    "12": "ST4_DEC_N", "13": "LX200_TX", "14": "LX200_RX",
    # SARADC: IN1 = NTC, IN0 = recovery key (U-Boot adc-keys channel 0)
    "15": "NTC_ADC", "16": "RECOVERY", "17": "VCC_1V8",
    "18": "USB_DN", "19": "USB_DP", "20": "VCC_3V3",
    # audio codec unused; its supply stays powered (notes section 12.2)
    "21": NC, "22": NC, "23": "VCC_1V8", "24": NC, "25": NC, "26": NC,
    # FSPI, SPI NAND at 3.3 V
    "27": "VCC_3V3", "28": "FSPI_D3", "29": "FSPI_D0", "30": "FSPI_D1",
    "31": "FSPI_D2", "32": "FSPI_CS0N", "33": "FSPI_CLK_SOC",
    "34": "VCC_DDR", "35": "VCC_DDR", "36": "VDD_0V9", "37": "DRAM_ZQ",
    "38": "VCC_1V8", "39": "VDD_0V9", "40": "VCC_DDR",
    "41": "GND", "42": NC, "43": "VCC_3V3", "44": "VDD_0V9",
    "45": "OSC_XIN", "46": "OSC_XOUT", "47": "VCC_1V8",
    "48": NC, "49": "DBG_TX", "50": "DBG_RX", "51": "VCC_3V3",
    # VCCIO6 at 1.8 V for sensor sync (slave mode) and sensor rail enables.
    # 52-57 all default pull-down: rails off, master mode, sync low.
    "52": "SEN_DVDD_EN", "53": "SEN_OVDD_EN", "54": "SEN_AVDD_EN",
    "55": "SEN_XVS", "56": "SEN_XMASTER", "57": "SEN_XHS",
    "58": "VDD_0V9", "59": "VCC_1V8",
    # 60-65 SDIO_M1, reserved for Wi-Fi later
    "60": NC, "61": NC, "62": NC, "63": NC, "64": NC, "65": NC,
    "66": "VCC_1V8",
    # Ethernet PHY unused; AVDD and EXTR kept as on every Luckfox board
    "67": NC, "68": NC, "69": NC, "70": NC, "71": "VCC_3V3", "72": "ETH_EXTR",
    "73": "VDD_0V9", "74": "VDD_0V9", "75": "VDD_0V9",
    # MIPI CSI-2 4 lanes + CK0 (CK1 unused in 4-lane mode)
    "76": "MIPI_D3N", "77": "MIPI_D3P", "78": NC, "79": NC,
    "80": "MIPI_D2N", "81": "MIPI_D2P", "82": "MIPI_D1N", "83": "MIPI_D1P",
    "84": "MIPI_CKN", "85": "MIPI_CKP", "86": "MIPI_D0N", "87": "MIPI_D0P",
    "88": "VCC_1V8", "89": "GND",
}

SOC = [
    {"kind": "ic", "ref": "U201", "part": "RV1103G1", "x": 640, "y": 760,
     "pins": SOC_PINS},
    # 24 MHz crystal, CL 12 pF (Luckfox: 18 pF caps, 1M across, 22R on XOUT)
    {"kind": "ic", "ref": "Y201", "part": "XTAL24M", "x": 1300, "y": 1060, "pins": {
        "1": "OSC_XIN", "3": "OSC_XOUT_R", "2": "GND", "4": "GND"}},
    {"kind": "rows", "x": 1300, "y": 1000, "pitch": 38, "items": [
        ("R202", "22R", "OSC_XOUT", "OSC_XOUT_R"),
        ("R203", "1M", "OSC_XIN", "OSC_XOUT"),
        ("C201", "18p", "OSC_XIN", "GND"),
        ("C202", "18p", "OSC_XOUT_R", "GND"),
        ("R201", "22R", "SEN_INCK_SOC", "SEN_INCK"),
        ("R204", "240R", "DRAM_ZQ", "GND"),
        ("R205", "6k04", "ETH_EXTR", "GND"),
        # SPI NAND: 22R on CLK, pull-ups on WP#/HOLD# (Luckfox)
        ("R206", "22R", "FSPI_CLK_SOC", "FSPI_CLK"),
        ("R207", "10k", "VCC_3V3", "FSPI_D2"),
        ("R208", "10k", "VCC_3V3", "FSPI_D3"),
        # Recovery key on SARADC_IN0 (Luckfox): always pulled up; short
        # the pad to GND at power-up for MaskRom / download mode.
        ("R209", "10k", "VCC_1V8", "RECOVERY"),
        ("C204", "1n", "RECOVERY", "GND"),
        ("R210", "100R", "RECOVERY", "RECOVERY_PAD"),
        # NTC bias on SARADC_IN1 (the NTC itself sits on the sensor page)
        ("R211", "10k", "VCC_1V8", "NTC_ADC"),
        ("C205", "1n", "NTC_ADC", "GND"),
        # Debug console UART2_M1 (Luckfox: 100R series)
        ("R212", "100R", "DBG_TX", "DBG_TX_PAD"),
        ("R213", "100R", "DBG_RX", "DBG_RX_PAD"),
    ]},
    {"kind": "ic", "ref": "U202", "part": "W25N01KV", "x": 1280, "y": 290, "pins": {
        "1": "FSPI_CS0N", "2": "FSPI_D1", "3": "FSPI_D2", "4": "GND",
        "5": "FSPI_D0", "6": "FSPI_CLK", "7": "FSPI_D3", "8": "VCC_3V3",
        "9": "GND"}},
    # Development pads, back side, not assembled: a row of 2.5 mm SMD pads.
    # Short RECOVERY to the GND pad while plugging in for MaskRom/download;
    # TX/RX/GND is the serial console. Exposed I/O stays a 2.54 mm header.
    {"kind": "ic", "ref": "TP201", "part": "TP", "bom": False,
     "x": 1580, "y": 1080, "pins": {"1": "RECOVERY_PAD"}},
    {"kind": "ic", "ref": "TP202", "part": "TP", "bom": False,
     "x": 1580, "y": 1040, "pins": {"1": "GND"}},
    {"kind": "ic", "ref": "TP203", "part": "TP", "bom": False,
     "x": 1580, "y": 1000, "pins": {"1": "DBG_TX_PAD"}},
    {"kind": "ic", "ref": "TP204", "part": "TP", "bom": False,
     "x": 1580, "y": 960, "pins": {"1": "DBG_RX_PAD"}},
    {"kind": "ic", "ref": "J203", "part": "HDR1x8", "bom": False,
     "x": 1560, "y": 760, "pins": {
        "1": "VCC_3V3", "2": "ST4_RA_P", "3": "ST4_RA_N", "4": "ST4_DEC_P",
        "5": "ST4_DEC_N", "6": "LX200_TX", "7": "LX200_RX", "8": "GND"}},
    # Decoupling per Luckfox Mini "POWER" section, plus 100n per IO supply
    {"kind": "decaps", "x": 80, "y": 420, "pitch": 50, "items": [
        ("C206", "10u", "VDD_0V9"), ("C207", "10u", "VDD_0V9"),
        ("C208", "10u", "VDD_0V9"), ("C209", "10u", "VDD_0V9"),
        ("C210", "1u", "VDD_0V9"), ("C211", "1u", "VDD_0V9"),
        ("C212", "1u", "VDD_0V9"), ("C213", "1u", "VDD_0V9"),
        ("C214", "100n", "VDD_0V9"), ("C215", "100n", "VDD_0V9"),
        ("C216", "100n", "VDD_0V9"), ("C217", "100n", "VDD_0V9"),
        ("C218", "10u", "VCC_DDR"), ("C219", "10u", "VCC_DDR"),
    ]},
    {"kind": "decaps", "x": 80, "y": 260, "pitch": 50, "items": [
        ("C220", "1u", "VCC_DDR"), ("C221", "100n", "VCC_DDR"),
        ("C222", "10u", "VCC_1V8"), ("C223", "1u", "VCC_1V8"),
        ("C224", "100n", "VCC_1V8"), ("C225", "100n", "VCC_1V8"),
        ("C226", "100n", "VCC_1V8"), ("C227", "100n", "VCC_1V8"),
        ("C228", "100n", "VCC_1V8"), ("C229", "100n", "VCC_1V8"),
        ("C230", "100n", "VCC_3V3"), ("C231", "100n", "VCC_3V3"),
        ("C232", "100n", "VCC_3V3"), ("C233", "100n", "VCC_3V3"),
    ]},
    {"kind": "decaps", "x": 80, "y": 100, "pitch": 50, "items": [
        ("C234", "100n", "VCC_3V3"), ("C203", "100n", "VCC_3V3"),
    ]},
]

# ================================================================ SENSOR
_AVDD, _OVDD, _DVDD = "SEN_AVDD_2V9", "SEN_OVDD_1V8", "SEN_DVDD_1V2"
SENSOR_PINS = {
    # supplies (Sony pin names in comments)
    "A4": _AVDD, "A6": _AVDD, "A7": _AVDD, "A8": _AVDD, "A9": _AVDD,  # VDDH*
    "M1": _OVDD,                                                       # VDDMIF
    "D11": _DVDD, "M11": _DVDD,                                        # VDDLCN
    "E4": _DVDD, "E8": _DVDD, "K5": _DVDD, "K8": _DVDD,                # VDDLSC
    "K6": _DVDD, "K7": _DVDD,                                          # VDDLIF
    # reference pins -> caps to GND
    "A3": "SEN_VLOADLM", "A5": "SEN_VRLFR", "B5": "SEN_VRLST", "C11": "SEN_VBGR",
    # control
    "J10": "GND",            # OMODE low = CSI-2
    "F11": "SEN_XMASTER", "K10": "SEN_XCLR", "G11": _OVDD,   # XCE tied high (I2C)
    "G10": "SEN_SCL", "H10": "SEN_SDA",
    "H11": "SEN_XVS", "L10": "SEN_XHS", "P3": "SEN_INCK",
    # CSI-2: Sony lane n -> SoC D(n-1), P->P, N->N
    "N6": "MIPI_D0P", "P6": "MIPI_D0N", "N8": "MIPI_D1P", "P8": "MIPI_D1N",
    "N5": "MIPI_D2P", "P5": "MIPI_D2N", "N9": "MIPI_D3P", "P9": "MIPI_D3N",
    "N7": "MIPI_CKP", "P7": "MIPI_CKN",
}
for _p in ("B3 B4 B6 B7 B8 B9 C3 D3 D4 D8 D10 E3 E10 K3 L3 L5 L6 L7 L8 L11 "
           "M2 M3 M5 M6 M7 M8 M9 M10 N3").split():
    SENSOR_PINS[_p] = "GND"
# Everything else stays open: N.C., the LVDS DLO*/DLCK* outputs (must float
# so an IMX290 fits the same footprint), TAMON, TOUT, TENABLE, SDO, XTRIG.

SENSOR = [
    {"kind": "ic", "ref": "U301", "part": "IMX290LQR", "x": 520, "y": 650,
     "pins": SENSOR_PINS, "unlisted": NC},
    # Sensor rails, each LDO with its own enable. Sequencing (DVDD -> OVDD
    # -> AVDD on, reverse off) is done in the device tree by chaining the
    # regulators' vin-supply -- the vendor imx327.c enables all three at once
    # -- and both LDO types' active output discharge makes the power-down
    # order real. 100k pull-downs keep the sensor unpowered until Linux
    # enables it.
    # DVDD carries the bulk of the sensor current, so it runs from the 1.8 V
    # rail (0.6 V drop) instead of 3.3 V (2.1 V drop, ~0.2 W in a SOT-23 next
    # to the sensor at an estimated 100 mA -- Sony gives no figure). That
    # needs an LDO rated down to 1.45 V in: TLV75512P (500 mA). LP5907
    # (2.2 V min in, 6.5 uVrms) stays on the analog AVDD and on OVDD. Same
    # SOT-23-5 pinout, so the parts swap if needed.
    {"kind": "ic", "ref": "U302", "part": "TLV75512", "x": 1000, "y": 1060, "pins": {
        "1": "VCC_1V8", "2": "GND", "3": "SEN_DVDD_EN", "4": NC, "5": _DVDD}},
    {"kind": "ic", "ref": "U303", "part": "LP5907_1V8", "x": 1000, "y": 960, "pins": {
        "1": "VCC_3V3", "2": "GND", "3": "SEN_OVDD_EN", "4": NC, "5": _OVDD}},
    {"kind": "ic", "ref": "U304", "part": "LP5907_2V9", "x": 1000, "y": 860, "pins": {
        "1": "VCC_3V3", "2": "GND", "3": "SEN_AVDD_EN", "4": NC, "5": _AVDD}},
    {"kind": "rows", "x": 1450, "y": 1080, "pitch": 40, "items": [
        ("R301", "100k", "SEN_DVDD_EN", "GND"),
        ("R302", "100k", "SEN_OVDD_EN", "GND"),
        ("R303", "100k", "SEN_AVDD_EN", "GND"),
        # I2C pull-ups to sensor OVDD (Sony: 1k)
        ("R304", "1k", _OVDD, "SEN_SCL"),
        ("R305", "1k", _OVDD, "SEN_SDA"),
        # reset held asserted and master mode selected until software acts
        ("R306", "10k", "SEN_XCLR", "GND"),
        ("R307", "10k", "SEN_XMASTER", "GND"),
        # NTC next to the sensor, biased from the SoC page (R211/C205)
        ("RT301", "NTC10k", "NTC_ADC", "GND"),
    ]},
    {"kind": "decaps", "x": 720, "y": 720, "pitch": 50, "items": [
        ("C301", "1u", "VCC_1V8"), ("C302", "1u", "VCC_3V3"),
        ("C303", "1u", "VCC_3V3"), ("C304", "1u", _DVDD),
        ("C305", "1u", _OVDD), ("C306", "1u", _AVDD),
        # reference pins (Sony values)
        ("C335", "1u", "SEN_VLOADLM"), ("C336", "1u", "SEN_VRLFR"),
        ("C337", "1u", "SEN_VRLST"), ("C338", "220n", "SEN_VBGR"),
        ("C317", "10n", _OVDD), ("C318", "4u7", _OVDD),
    ]},
    # Per-pin decoupling from the IMX290 peripheral-circuit page
    # (notes section 2.2): AVDD 0.1u || 4.7u (A7: 0.1u || 1u),
    # DVDD VDDLCN/VDDLSC 0.1u || 4.7u, VDDLIF 0.01u || 1u.
    {"kind": "decaps", "x": 720, "y": 540, "pitch": 50, "items": [
        ("C307", "100n", _AVDD), ("C308", "100n", _AVDD), ("C309", "100n", _AVDD),
        ("C310", "100n", _AVDD), ("C311", "100n", _AVDD), ("C312", "1u", _AVDD),
        ("C313", "4u7", _AVDD), ("C314", "4u7", _AVDD), ("C315", "4u7", _AVDD),
        ("C316", "4u7", _AVDD),
    ]},
    {"kind": "decaps", "x": 720, "y": 360, "pitch": 50, "items": [
        ("C319", "100n", _DVDD), ("C320", "100n", _DVDD), ("C321", "100n", _DVDD),
        ("C322", "100n", _DVDD), ("C323", "100n", _DVDD), ("C324", "100n", _DVDD),
        ("C325", "4u7", _DVDD), ("C326", "4u7", _DVDD), ("C327", "4u7", _DVDD),
        ("C328", "4u7", _DVDD), ("C329", "4u7", _DVDD), ("C330", "4u7", _DVDD),
        ("C331", "10n", _DVDD), ("C332", "10n", _DVDD),
        ("C333", "1u", _DVDD), ("C334", "1u", _DVDD),
    ]},
]

PAGES = {
    "Power": ("c70f0139b86dc51c", POWER),
    "SoC": ("8f99986f727f554f", SOC),
    "Sensor": ("497abef3955e9dad", SENSOR),
}
