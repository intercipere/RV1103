#!/usr/bin/env python3
"""Draw design.py into the open EasyEDA project via the bridge.

  python3 generate.py [Power|SoC|Sensor ...]    (default: all pages)

Each page is cleared first (everything except the drawing frame), so this is
idempotent. Connectivity is by net name: every pin gets a short wire stub
carrying its net; GND on a downward pin gets a ground flag; NC pins get a
no-connect mark. Check the result with `check.py`.
"""
import json
import sys
import time
import urllib.request

import design

BRIDGE = "http://127.0.0.1:49620/execute"
PROJECT = "30262d33d2be4b43ba537458e0842f5a"


def run(code, timeout=60):
    req = urllib.request.Request(
        BRIDGE, data=json.dumps({"code": code}).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        res = json.loads(r.read())
    if not res.get("success"):
        raise RuntimeError(res.get("error"))
    return res["result"]


def resolve_parts():
    ids = sorted(set(design.P.values()))
    rows = run("const r = await eda.lib_Device.getByLcscIds(%s); "
               "return r.map(d=>[d.supplierId, d.libraryUuid, d.uuid]);" % json.dumps(ids))
    found = {lcsc: {"libraryUuid": lib, "uuid": uuid} for lcsc, lib, uuid in rows}
    missing = [i for i in ids if i not in found]
    if missing:
        raise SystemExit("not in library: %s" % missing)
    return {k: found[v] for k, v in design.P.items()}


# JS helpers shared by every request. stub(): wire from a pin end outward,
# carrying the net name. The wire's net label is a "Name" attribute whose
# (x, y) is where the text starts; text runs right (horizontal) or up
# (vertical). Labels are re-anchored so they never cover pin numbers:
# right/up stubs start the text just past the pin end, left/down stubs
# start it at the far end and are made long enough for the text.
# Two API quirks (EasyEDA Pro, 2026-10): attribute modify() negates y, and
# ignores alignMode -- hence the -y below.
JS_HELPERS = r"""
const DIR = {0:[1,0], 90:[0,1], 180:[-1,0], 270:[0,-1]};
async function relabel(wire, x, y, visible) {
  const attrs = await eda.sch_PrimitiveAttribute.getAll(wire.getState_PrimitiveId());
  const a = attrs.find(t => t.key === 'Name');
  if (!a) return;
  const p = visible ? {x: x, y: -y} : {valueVisible: false};
  await eda.sch_PrimitiveAttribute.modify(a.primitiveId, p);
}
async function stub(x, y, rot, net) {
  const d = DIR[((rot % 360) + 360) % 360];
  if (net === 'GND' && d[1] === -1) {
    const w = await eda.sch_PrimitiveWire.create([x, y, x, y - 10], 'GND');
    await eda.sch_PrimitiveComponent.createNetFlag('Ground', 'GND', x, y - 10, 0, false);
    await relabel(w, 0, 0, false);
    return;
  }
  const back = (d[0] === -1 || d[1] === -1);
  const L = back ? Math.max(30, Math.ceil((4.1 * net.length + 21) / 10) * 10) : 30;
  const ex = x + d[0] * L, ey = y + d[1] * L;
  const w = await eda.sch_PrimitiveWire.create([x, y, ex, ey], net);
  if (d[1] === 0) await relabel(w, back ? ex + 3 : x + 10, y + 1, true);
  else await relabel(w, x - 1, back ? ey + 3 : y + 10, true);
}
async function place(dev, x, y, rot, ref, bom) {
  const c = await eda.sch_PrimitiveComponent.create(dev, x, y, '', rot, false, bom, true);
  if (!c) throw new Error('create failed ' + ref);
  const id = c.getState_PrimitiveId();
  // Stable unique ID per designator: EasyEDA matches schematic parts to PCB
  // footprints by it, so a regenerated page maps onto the placed footprints.
  for (let i = 0; i < 3; i++) {
    await eda.sch_PrimitiveComponent.modify(id, {designator: ref, uniqueId: 'gge' + ref});
    const back = await eda.sch_PrimitiveComponent.get(id);
    if (back && back.getState_Designator() === ref && back.getState_UniqueId() === 'gge' + ref) return id;
  }
  throw new Error('designator did not stick: ' + ref);
}
"""


def open_page(uuid):
    run("await eda.dmt_EditorControl.openDocument('%s'); return true;" % uuid)


def clear_page(uuid):
    open_page(uuid)
    return run("""
const comps = await eda.sch_PrimitiveComponent.getAll();
const ids = comps.filter(c => c.getState_ComponentType() !== 'sheet').map(c => c.getState_PrimitiveId());
for (let i = 0; i < ids.length; i += 50) await eda.sch_PrimitiveComponent.delete(ids.slice(i, i + 50));
const ws = await eda.sch_PrimitiveWire.getAll();
const wids = ws.map(w => w.getState_PrimitiveId());
for (let i = 0; i < wids.length; i += 100) await eda.sch_PrimitiveWire.delete(wids.slice(i, i + 100));
return [ids.length, wids.length];""", timeout=120)


def place_ic(blk, devs):
    dev = devs[blk["part"]]
    bom = blk.get("bom", True)
    res = run(JS_HELPERS + """
const id = await place(%s, %d, %d, %d, '%s', %s);
const pins = await eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId(id);
return {id, pins: pins.map(p => [p.pinNumber, p.x, p.y, p.rotation])};"""
              % (json.dumps(dev), blk["x"], blk["y"], blk.get("rot", 0), blk["ref"],
                 "true" if bom else "false"))
    cid, pins = res["id"], res["pins"]
    nets = blk["pins"]
    numbers = [p[0] for p in pins]
    unknown = set(nets) - set(numbers)
    if unknown:
        raise SystemExit("%s: pins not on symbol: %s" % (blk["ref"], sorted(unknown)))
    todo, ncs = [], []
    for num, x, y, rot in pins:
        if num in nets:
            net = nets[num]
        elif "unlisted" in blk:
            net = blk["unlisted"]
        else:
            raise SystemExit("%s: pin %s has no net assigned" % (blk["ref"], num))
        if net is None:
            ncs.append(num)
        else:
            todo.append((x, y, rot, net))
    for i in range(0, len(todo), 25):
        calls = "".join("await stub(%d,%d,%d,%s);\n" % (x, y, r, json.dumps(n))
                        for x, y, r, n in todo[i:i + 25])
        run(JS_HELPERS + calls + "return true;")
    if ncs:
        run("""
const pins = await eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId('%s');
const nc = new Set(%s);
for (const p of pins) if (nc.has(p.pinNumber)) { p.setState_NoConnected(true); await p.done(); }
return true;""" % (cid, json.dumps(ncs)))
    return len(todo), len(ncs)


def place_two_pin(items, devs, vertical):
    """items: (ref, part, x, y, net_a, net_b). Horizontal: a = left pin.
    Vertical: a = top pin, b = bottom pin."""
    for i in range(0, len(items), 5):
        chunk = items[i:i + 5]
        body = ""
        for ref, part, x, y, na, nb in chunk:
            body += """
{ const id = await place(%s, %d, %d, %d, '%s', true);
  const pins = await eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId(id);
  for (const p of pins) {
    const r = ((p.rotation %% 360) + 360) %% 360;
    const net = (r === %d) ? %s : %s;
    await stub(p.x, p.y, r, net);
  } }""" % (json.dumps(devs[part]), x, y, 90 if vertical else 0, ref,
            90 if vertical else 180, json.dumps(na), json.dumps(nb))
        run(JS_HELPERS + body + "\nreturn true;")


def draw_page(name):
    uuid, blocks = design.PAGES[name]
    devs = resolve_parts()
    n = clear_page(uuid)
    print("%s: cleared %d parts, %d wires" % (name, n[0], n[1]))
    for blk in blocks:
        t = time.time()
        if blk["kind"] == "ic":
            s, c = place_ic(blk, devs)
            print("  %-6s %-10s %3d nets %3d NC  %.1fs" % (blk["ref"], blk["part"], s, c, time.time() - t))
        elif blk["kind"] == "rows":
            items = [(ref, part, blk["x"], blk["y"] - i * blk["pitch"], a, b)
                     for i, (ref, part, a, b) in enumerate(blk["items"])]
            place_two_pin(items, devs, vertical=False)
            print("  rows   %2d parts  %.1fs" % (len(items), time.time() - t))
        elif blk["kind"] == "decaps":
            items = [(ref, part, blk["x"] + i * blk["pitch"], blk["y"], net, "GND")
                     for i, (ref, part, net) in enumerate(blk["items"])]
            place_two_pin(items, devs, vertical=True)
            print("  decaps %2d parts  %.1fs" % (len(items), time.time() - t))
    run("await eda.sch_Document.save(); return true;")


if __name__ == "__main__":
    for page in sys.argv[1:] or list(design.PAGES):
        draw_page(page)
