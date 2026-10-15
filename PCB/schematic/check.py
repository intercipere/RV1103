#!/usr/bin/env python3
"""Compare EasyEDA's netlist against design.py, pin by pin.

Reports: pins whose net differs from the spec, pins missing from the
netlist, nets with a single connection (likely a typo), and a summary.
Run after generate.py.
"""
import collections
import re
import sys

import design
from generate import run


def expected():
    exp = {}
    for _, blocks in design.PAGES.values():
        for blk in blocks:
            if blk["kind"] == "ic":
                for num, net in blk["pins"].items():
                    if net is not None:
                        exp[(blk["ref"], num)] = net
            elif blk["kind"] == "rows":
                for ref, _, a, b in blk["items"]:
                    exp[(ref, "L")], exp[(ref, "R")] = a, b
            elif blk["kind"] == "decaps":
                for ref, _, net in blk["items"]:
                    exp[(ref, "T")], exp[(ref, "B")] = net, "GND"
    return exp


def actual():
    text = run("return await eda.sch_Netlist.getNetlist('Protel2');", timeout=120)
    nets = {}
    for m in re.finditer(r"\(\s*\n(.*?)\n\)", text, re.S):
        lines = [l.strip() for l in m.group(1).splitlines() if l.strip()]
        name, conns = lines[0], lines[1:]
        nets[name] = [c.split()[0] for c in conns]      # "U201-45"
    return nets


def two_pin_sides():
    """Map (ref, pin number) -> L/R/T/B for two-pin parts, from the drawing."""
    code = """
const out = {};
for (const page of %s) {
  await eda.dmt_EditorControl.openDocument(page);
  const comps = await eda.sch_PrimitiveComponent.getAll();
  for (const c of comps) {
    if (c.getState_ComponentType() !== 'part') continue;
    const pins = await eda.sch_PrimitiveComponent.getAllPinsByPrimitiveId(c.getState_PrimitiveId());
    if (pins.length !== 2) continue;
    out[c.getState_Designator()] = pins.map(p => [p.pinNumber, p.rotation]);
  }
}
return out;""" % [uuid for uuid, _ in design.PAGES.values()]
    sides = {}
    for ref, pins in run(code, timeout=180).items():
        for num, rot in pins:
            rot %= 360
            sides[(ref, num)] = {0: "R", 180: "L", 90: "T", 270: "B"}[rot]
    return sides


def main():
    exp = expected()
    nets = actual()
    sides = two_pin_sides()
    # rows/decaps parts are checked by side (L/R/T/B), everything else by
    # pin number
    by_side = {ref for (ref, s) in exp if s in ("L", "R", "T", "B")}
    got = {}
    for net, conns in nets.items():
        for c in conns:
            ref, num = c.rsplit("-", 1)
            key = (ref, sides[(ref, num)]) if ref in by_side else (ref, num)
            got[key] = net
    errors = 0
    for key, net in sorted(exp.items()):
        if key not in got:
            print("MISSING   %-6s %-6s expected %s" % (key[0], key[1], net))
            errors += 1
        elif got[key] != net:
            print("WRONG     %-6s %-6s expected %s, got %s" % (key[0], key[1], net, got[key]))
            errors += 1
    for key, net in sorted(got.items()):
        if key not in exp:
            print("EXTRA     %-6s %-6s on %s" % (key[0], key[1], net))
            errors += 1
    singles = [n for n, c in nets.items() if len(c) == 1]
    for n in sorted(singles):
        print("SINGLE    net %s has one connection: %s" % (n, nets[n]))
    parts = {k[0] for k in exp}
    print("\n%d nets, %d connections, %d parts with nets, %d mismatches, %d single-pin nets"
          % (len(nets), sum(len(c) for c in nets.values()), len(parts), errors, len(singles)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
