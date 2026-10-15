#!/usr/bin/env python3
"""Initial component placement for the OpenAstroGuider board (EasyEDA PCB1).

  python3 place.py            place ICs/connectors, then pack passives

Board: 40 x 40 mm centred on (0,0) = sensor optical axis; M2.5 holes at
(+-15, +-15). Coordinates here are mm, top view (y up); EasyEDA uses mil.

Stage A puts the ICs/connectors at hand-picked spots (IC_PLACEMENT).
Stage B packs every two-pin part as close as possible to the pin it serves
(ANCHORS), trying rotation 0/90 on a spiral search, avoiding other parts,
hole keep-outs, the board edge and reserved zones. It is a first pass for
review, not a final placement.
"""
import json
import math
import sys

sys.path.insert(0, "../schematic")
from generate import run   # noqa: E402

MIL = 1 / 0.0254
TOP, BOTTOM = 1, 2
HOLES = [(-15, -15), (15, -15), (15, 15), (-15, 15)]
HOLE_KEEPOUT = 3.5          # mm radius around hole centres (M2.5 head + margin)
EDGE = 0.5                  # mm keep-in from the outline
GAP = 0.15                  # mm between part bounding boxes

# ---------------------------------------------------------------- stage A
# ref: (layer, x, y, rotation). Rationale in HARDWARE-DESIGN-NOTES.md sec 14.
OPT_DX, OPT_DY = 0.058, -0.235   # optical centre in U301 footprint frame (mm)
IC_PLACEMENT = {
    # Sensor alone on the PCB bottom layer (= camera front, lens side);
    # everything else on top. On the bottom layer the footprint is flipped in
    # y, so the optical centre (+0.058, -0.235 mm in footprint coordinates)
    # lands at (+0.058, +0.235) from the origin -> origin at (-0.058, -0.235).
    "U301": (BOTTOM, -OPT_DX, OPT_DY, 0),
    # SoC, rotation 90: MIPI pins 76-87 along its left edge facing the
    # sensor's N/P columns; MCLK/I2C (1-5) at its bottom-left corner, next to
    # the sensor's control pins (rows 10/11, now at the sensor's lower edge).
    "U201": (TOP, 11.5, 1.0, 90),
    "U202": (TOP, 10.3, -8.6, 0),        # SPI NAND below, FSPI is the right edge
    "Y201": (TOP, 17.6, 6.6, 0),         # OSC pins 45/46 at the top-right corner
    # USB-C (vertical), ESD, buck converter on the left
    "J101": (TOP, -12.0, 8.0, 0),
    "U101": (TOP, -12.0, 3.2, 90),
    "U102": (TOP, -12.5, -5.5, 0),
    # sensor LDOs above the sensor (AVDD towards the sensor's A column)
    "U304": (TOP, -5.0, 8.2, 0),
    "U302": (TOP, -0.5, 8.2, 0),
    "U303": (TOP, 3.8, 8.2, 0),
    # I/O header along the top edge, between the holes
    "J203": (TOP, 0.0, 17.3, 0),
    # dew heater: XH at the bottom, opening facing up into the board
    "J102": (TOP, 0.0, -17.4, 180),
    "Q101": (TOP, 6.5, -15.8, 0),
    # dev pads, bottom left
    "TP201": (TOP, -6.3, -17.6, 0), "TP202": (TOP, -10.0, -17.6, 0),
    "TP203": (TOP, -6.3, -13.9, 0), "TP204": (TOP, -10.0, -13.9, 0),
}
# Free space in front of the XH opening for the plug (component side).
RESERVED = [(-3.6, -10.0, 3.6, -2.2)]      # (x0, y0, x1, y1) mm

# ---------------------------------------------------------------- stage B
# passive -> (ref, pad) it should sit next to, in placement priority order.
def _sensor_caps():
    pins = {"C307": "A4", "C308": "A6", "C309": "A7", "C310": "A8", "C311": "A9",
            "C312": "A7", "C313": "A4", "C314": "A6", "C315": "A8", "C316": "A9",
            "C319": "D11", "C320": "M11", "C321": "E4", "C322": "E8",
            "C323": "K5", "C324": "K8", "C325": "D11", "C326": "M11",
            "C327": "E4", "C328": "E8", "C329": "K5", "C330": "K8",
            "C331": "K6", "C332": "K7", "C333": "K6", "C334": "K7",
            "C317": "M1", "C318": "M1",
            "C335": "A3", "C336": "A5", "C337": "B5", "C338": "C11"}
    return [(c, ("U301", p)) for c, p in pins.items()]


def _soc_caps():
    # 100n first, one per supply pin; then 1u/10u spread over the same rails
    vdd = ["6", "36", "39", "44", "58", "73", "74", "75"]
    ddr = ["34", "35", "40"]
    v18 = ["17", "23", "38", "47", "59", "66", "88"]
    v33 = ["7", "20", "27", "43", "51", "71"]
    out = []
    out += list(zip(["C214", "C215", "C216", "C217"], [("U201", p) for p in vdd[:4]]))
    out += [("C221", ("U201", ddr[0]))]
    out += list(zip(["C224", "C225", "C226", "C227", "C228", "C229"], [("U201", p) for p in v18[:6]]))
    out += list(zip(["C230", "C231", "C232", "C233", "C234"], [("U201", p) for p in v33[:5]]))
    out += [("C203", ("U202", "8"))]
    out += list(zip(["C210", "C211", "C212", "C213"], [("U201", p) for p in vdd[4:]]))
    out += list(zip(["C206", "C207", "C208", "C209"], [("U201", p) for p in vdd[::2]]))
    out += [("C220", ("U201", ddr[1])), ("C218", ("U201", ddr[2])), ("C219", ("U201", ddr[0]))]
    out += [("C223", ("U201", v18[6])), ("C222", ("U201", v18[1]))]
    return out


ANCHORS = (
    _sensor_caps()
    + [("RT301", ("U301", "K6"))]
    + _soc_caps()
    + [("R202", ("Y201", "3")), ("C201", ("Y201", "1")), ("C202", ("Y201", "3")),
       ("R203", ("U201", "45")), ("R201", ("U201", "1")), ("R204", ("U201", "37")),
       ("R205", ("U201", "72")), ("R206", ("U201", "33")),
       ("R207", ("U202", "3")), ("R208", ("U202", "7")),
       ("R209", ("U201", "16")), ("C204", ("U201", "16")), ("R210", ("TP201", "1")),
       ("R211", ("U201", "15")), ("C205", ("U201", "15")),
       ("R212", ("TP203", "1")), ("R213", ("TP204", "1"))]
    # sensor rails and strapping
    + [("C301", ("U302", "1")), ("C304", ("U302", "5")), ("R301", ("U302", "3")),
       ("C302", ("U303", "1")), ("C305", ("U303", "5")), ("R302", ("U303", "3")),
       ("C303", ("U304", "1")), ("C306", ("U304", "5")), ("R303", ("U304", "3")),
       ("R304", ("U301", "G10")), ("R305", ("U301", "H10")),
       ("R306", ("U301", "K10")), ("R307", ("U301", "F11"))]
    # power
    + [("L101", ("U102", "11")), ("L102", ("U102", "6")), ("L103", ("U102", "20")),
       ("C108", ("U102", "10")), ("C109", ("U102", "7")), ("C110", ("U102", "19")),
       ("C111", ("U102", "16")),
       ("R103", ("U102", "14")), ("R104", ("U102", "14")), ("C103", ("U102", "14")),
       ("R105", ("U102", "3")), ("R106", ("U102", "3")), ("C104", ("U102", "3")),
       ("R107", ("U102", "2")), ("R108", ("U102", "2")), ("C105", ("U102", "2")),
       ("R109", ("U102", "8")), ("R110", ("U102", "9")), ("C106", ("U102", "9")),
       ("R111", ("U102", "18")), ("C107", ("U102", "18")),
       ("C112", ("L101", "2")), ("C113", ("L101", "2")),
       ("C114", ("L102", "2")), ("C115", ("L102", "2")),
       ("C116", ("L103", "2")), ("C117", ("L103", "2")),
       ("R112", ("L103", "2")),
       ("C101", ("J101", "A4")), ("C102", ("J101", "A4")),
       ("R101", ("J101", "A5")), ("R102", ("J101", "B5")),
       ("R113", ("Q101", "1")), ("R114", ("Q101", "1"))]
)


def state():
    """Absolute bbox and pad positions (mm) of every component."""
    code = """
const comps = await eda.pcb_PrimitiveComponent.getAll();
const pads = await eda.pcb_PrimitivePad.getAll();
const out = {}, byId = {};
for (const c of comps) {
  const id = c.getState_PrimitiveId();
  const b = await eda.pcb_Primitive.getPrimitivesBBox([id]);
  byId[id] = c.getState_Designator();
  out[byId[id]] = {id, x: c.getState_X(), y: c.getState_Y(), rot: c.getState_Rotation(),
                   layer: c.getState_Layer(), bb: [b.minX, b.minY, b.maxX, b.maxY], pads: {}, tht: false};
}
for (const p of pads) {
  const ref = byId[p.primitiveId.slice(0, 16)];
  if (!ref) continue;
  out[ref].pads[p.padNumber] = [p.x, p.y];
  if (p.hole) out[ref].tht = true;
}
return out;"""
    raw = run(code, timeout=300)
    for c in raw.values():
        c["bb"] = [v / MIL for v in c["bb"]]
        c["pads"] = {k: (v[0] / MIL, v[1] / MIL) for k, v in c["pads"].items()}
        c["x"] /= MIL
        c["y"] /= MIL
    return raw


def move(ref, cid, layer, x, y, rot):
    run("await eda.pcb_PrimitiveComponent.modify('%s', {layer: %d, x: %f, y: %f, rotation: %d}); return true;"
        % (cid, layer, x * MIL, y * MIL, rot))


def overlaps(a, b):
    return not (a[2] + GAP <= b[0] or b[2] + GAP <= a[0] or a[3] + GAP <= b[1] or b[3] + GAP <= a[1])


def legal(bb, obstacles):
    if bb[0] < -20 + EDGE or bb[2] > 20 - EDGE or bb[1] < -20 + EDGE or bb[3] > 20 - EDGE:
        return False
    for hx, hy in HOLES:   # distance from hole centre to the box
        dx = max(bb[0] - hx, 0, hx - bb[2])
        dy = max(bb[1] - hy, 0, hy - bb[3])
        if math.hypot(dx, dy) < HOLE_KEEPOUT:
            return False
    return not any(overlaps(bb, o) for o in obstacles)


def spiral(cx, cy, rmax=14.0, step=0.1):
    yield cx, cy
    r = step
    while r <= rmax:
        n = max(8, int(2 * math.pi * r / step))
        for i in range(n):
            a = 2 * math.pi * i / n
            yield round(cx + r * math.cos(a), 2), round(cy + r * math.sin(a), 2)
        r += step


def main():
    st = state()
    for ref, (layer, x, y, rot) in IC_PLACEMENT.items():
        move(ref, st[ref]["id"], layer, x, y, rot)
    st = state()

    # local half-extents of each passive at rotation 0 (from its current bbox)
    def half(ref):
        c = st[ref]
        w, h = c["bb"][2] - c["bb"][0], c["bb"][3] - c["bb"][1]
        return (h / 2, w / 2) if c["rot"] % 180 == 90 else (w / 2, h / 2)

    obstacles = [c["bb"] for r, c in st.items()
                 if r in IC_PLACEMENT and (c["layer"] == TOP or c["tht"])]
    obstacles += [list(z) for z in RESERVED]
    placed = {}
    failed = []
    for ref, (aref, apad) in ANCHORS:
        if aref in placed:      # anchor is a passive placed earlier: its pad pos
            ax, ay = placed[aref]["pads"][apad]
        else:
            ax, ay = st[aref]["pads"][apad]
        hw, hh = half(ref)
        best = None
        for x, y in spiral(ax, ay):
            for rot, (w, h) in ((0, (hw, hh)), (90, (hh, hw))):
                bb = [x - w, y - h, x + w, y + h]
                if legal(bb, obstacles):
                    best = (x, y, rot, bb)
                    break
            if best:
                break
        if not best:
            failed.append(ref)
            continue
        x, y, rot, bb = best
        move(ref, st[ref]["id"], TOP, x, y, rot)
        obstacles.append(bb)
        # parts other passives anchor to: read their real (mirrored) pads back
        pads = {}
        if any(a[0] == ref for _, a in ANCHORS):
            raw = run("const id = '%s'; const ps = await eda.pcb_PrimitivePad.getAll(); "
                      "return ps.filter(p => p.primitiveId.startsWith(id)).map(p => [p.padNumber, p.x, p.y]);"
                      % st[ref]["id"], timeout=120)
            pads = {n: (px / MIL, py / MIL) for n, px, py in raw}
        placed[ref] = {"pads": pads}
        print("%-6s -> (%6.2f, %6.2f) rot %3d   %.2f mm from %s.%s"
              % (ref, x, y, rot, math.hypot(x - ax, y - ay), aref, apad))
    leftovers = [r for r in st if r not in IC_PLACEMENT and r not in placed]
    print("\nplaced %d passives; failed: %s; not anchored: %s" % (len(placed), failed, leftovers))
    run("await eda.pcb_Document.save(); return true;")


if __name__ == "__main__":
    main()
