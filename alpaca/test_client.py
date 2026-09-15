#!/usr/bin/env python3
"""
Real ASCOM Alpaca client test for alpacad, using ASCOM's own official
alpyca library (not hand-crafted curl calls) -- exercises discovery and the
Camera interface exactly as N.I.N.A./PHD2 would.

Requires alpacad running on the board and this host having an IP on the
usb0 RNDIS interface (see alpaca/README.md), e.g.:
    sudo ip addr add 172.32.0.1/16 dev <rndis-iface>

Usage:
    python3 test_client.py                # discover, then test
    python3 test_client.py 172.32.0.70    # skip discovery, connect directly
"""
import sys
import time

from alpaca import discovery
from alpaca.camera import Camera

DEVICE_NUMBER = 0


def find_device():
    print("Broadcasting Alpaca discovery (alpacadiscovery1, UDP :32227)...")
    found = discovery.search_ipv4(numquery=3, timeout=3)
    if not found:
        sys.exit("No Alpaca devices found via discovery. Is alpacad running, "
                  "and does this host have an IP on the RNDIS interface?")
    print(f"  found: {found}")
    return found[0].split(":")[0]


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else find_device()
    print(f"\nConnecting to camera at {host}:11111 ...")
    cam = Camera(f"{host}:11111", DEVICE_NUMBER)

    print(f"Name:        {cam.Name}")
    print(f"Description: {cam.Description}")
    print(f"SensorType:  {cam.SensorType}")
    print(f"Size:        {cam.CameraXSize} x {cam.CameraYSize}")
    print(f"MaxADU:      {cam.MaxADU}")
    print(f"ExposureMin/Max: {cam.ExposureMin:.6f} / {cam.ExposureMax:.6f} s")

    cam.Connected = True
    print(f"\nConnected: {cam.Connected}")

    duration = 0.05
    print(f"\nStartExposure(Duration={duration}, Light=True) ...")
    cam.StartExposure(duration, True)

    while not cam.ImageReady:
        print(f"  CameraState={cam.CameraState}, waiting...")
        time.sleep(0.3)

    print("Image ready. Fetching ImageArray via alpyca (validates the "
          "element ordering this project's README flagged as unverified)...")
    t0 = time.time()
    img = cam.ImageArray
    dt = time.time() - t0

    # alpyca returns a nested list or numpy array depending on install extras;
    # normalize just enough to report shape/stats without assuming which.
    try:
        import numpy as np
        arr = np.array(img)
        print(f"shape={arr.shape} dtype={arr.dtype} "
              f"min={arr.min()} max={arr.max()} mean={arr.mean():.1f} "
              f"(fetched in {dt:.2f}s)")
        expected = (cam.CameraXSize, cam.CameraYSize)
        if arr.shape[:2] not in (expected, expected[::-1]):
            print(f"  NOTE: shape {arr.shape[:2]} doesn't match "
                  f"CameraXSize/YSize {expected} in either order -- "
                  f"check send_imagearray()'s row/col nesting")
        elif arr.shape[:2] == expected[::-1]:
            print(f"  shape is (CameraYSize, CameraXSize) = row-major "
                  f"[row][col] -- matches what camera_api.c currently emits")
        else:
            print(f"  shape is (CameraXSize, CameraYSize) -- OPPOSITE of "
                  f"what camera_api.c currently emits, needs a fix")
    except ImportError:
        print(f"rows={len(img)} cols={len(img[0])} (install numpy for stats)")

    cam.Connected = False
    print("\nOK")


if __name__ == "__main__":
    main()
