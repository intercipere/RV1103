"""Pulls one frame off the camera via Alpaca ImageBytes and writes it as raw uint16.

ImageBytes layout (from ASCOM's own alpyca client): 11 little-endian int32 header
fields, then the pixel data starting at byte offset `dataStart`. Wire order is
[Width][Height] with X as the outer index.
"""
import struct, sys, time, urllib.request, urllib.parse

BASE = "http://localhost:11111/api/v1/camera/0"

def put(member, **kw):
    kw.setdefault("ClientID", "1"); kw.setdefault("ClientTransactionID", "1")
    data = urllib.parse.urlencode(kw).encode()
    req = urllib.request.Request(f"{BASE}/{member}", data=data, method="PUT")
    return urllib.request.urlopen(req, timeout=180).read()

def get(member):
    with urllib.request.urlopen(f"{BASE}/{member}?ClientID=1&ClientTransactionID=1", timeout=180) as r:
        import json; return json.loads(r.read())["Value"]

def grab(exposure, out):
    put("startexposure", Duration=str(exposure), Light="true")
    t0 = time.time()
    while not get("imageready"):
        time.sleep(0.05)
        if time.time() - t0 > 180:
            sys.exit("timed out waiting for imageready")
    req = urllib.request.Request(f"{BASE}/imagearray?ClientID=1&ClientTransactionID=1")
    req.add_header("Accept", "application/imagebytes")
    raw = urllib.request.urlopen(req, timeout=180).read()

    hdr = struct.unpack("<11i", raw[:44])
    (_mv, err, _ctid, _stid, data_start, elem_t, trans_t, rank, d1, d2, _d3) = hdr
    if err:
        sys.exit(f"alpaca error {err}")
    px = raw[data_start:]
    n = d1 * d2
    assert trans_t == 8, f"expected UInt16 transmission, got {trans_t}"
    assert len(px) >= n * 2, f"short data: {len(px)} < {n*2}"
    open(out, "wb").write(px[: n * 2])
    vals = struct.unpack(f"<{n}H", px[: n * 2])
    print(f"{out}: {d1}x{d2} rank={rank} elem={elem_t}/{trans_t} "
          f"exposure={exposure}s min={min(vals)} max={max(vals)} mean={sum(vals)/n:.1f}")
    return d1, d2

if __name__ == "__main__":
    exposure = float(sys.argv[1]); out = sys.argv[2]
    grab(exposure, out)
