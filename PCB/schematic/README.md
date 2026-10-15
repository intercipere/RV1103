# Schematic as data

The EasyEDA schematic of project `OAG_IMX327` (pages **Power**, **SoC**, **Sensor**, A3) is generated
from `design.py`. **Edit `design.py`, not the drawing**, then regenerate — a hand edit in EasyEDA is
overwritten on the next run of that page.

| File | Role |
|---|---|
| `design.py` | Every part (LCSC number), value, pin assignment and net, with the reasoning in comments |
| `generate.py` | Draws it through the EasyEDA bridge (`python3 generate.py [Power SoC Sensor]`); clears each page first, so it is idempotent |
| `check.py` | Pulls EasyEDA's own netlist and compares it pin by pin with `design.py`; also flags single-pin nets |

Prerequisites: the EasyEDA bridge running and the project open (see CLAUDE.md, "PCB design: Claude
Code <-> EasyEDA Pro bridge"). Page UUIDs are fixed in `design.PAGES`.

Status (2026-10-14): all three pages drawn; `check.py` reports 75 nets, 473 connections, 0
mismatches, 0 single-pin nets; EasyEDA strict schematic DRC 0 errors. 139 placed parts, 37 unique
(21 JLC basic, 16 extended), plus three unassembled header footprints (recovery, debug UART, I/O).

## Drawing conventions

- Connectivity is **by net name only**: every pin gets a short wire stub carrying its net and a label;
  downward GND pins get a ground flag; unused pins get a no-connect mark. There are no hand-routed
  wires, so every connection is visible as a name and checkable by `check.py`.
- Designators by page: 1xx Power, 2xx SoC, 3xx Sensor.
- Two-pin parts are either `rows` (horizontal, left pin = first net) or `decaps` (vertical, net on top,
  GND below).

## EasyEDA API quirks found while building this (EasyEDA Pro, 2026-10)

- A wire created with a net name shows that name as a label, anchored at the **wire midpoint**. Its
  position is a `Name` attribute on the wire; `(x, y)` is where the text *starts*, and text runs right
  (horizontal) or up (vertical).
- `SCH_PrimitiveAttribute.modify()` **negates y** (pass `-y`) and **ignores `alignMode`**.
- Pin `rotation` is the pin's outward direction: 0 = +x, 90 = +y, 180 = −x, 270 = −y. Y grows upward.
- Net flags (`createNetFlag`) connect exactly at their placement point.
- No-connect: `pin.setState_NoConnected(true); await pin.done()` on pins from
  `getAllPinsByPrimitiveId`.
- Page size: `modifySchematicPageTitleBlock` returns true but changes nothing. What works is deleting
  the page's `sheet` component and placing `Drawing-Symbol_A3` (system library) at (0, 0). That call
  can exceed the bridge's 30 s timeout while still completing.
- Enums such as `ESYS_NetlistType` are not globals in the bridge context; pass the string value
  (`'Protel2'`).
- Bridge requests time out at 30 s; `generate.py` batches ~25 stubs or 5 parts per request.
