#!/usr/bin/env python3
"""
One-command capture from the Luckfox board (RV1103 + SC3336).

Sets the sensor controls, captures one or more frames, pulls them to this PC,
unpacks the RAW10 data and writes a PNG preview.

Everyday use:
    python grab.py -e 20000 -g 128 --mono bin      # clean greyscale
    python grab.py -e 20000 -g 128 -n 8 --mono bin # averaged, less noise
    python grab.py --sweep 2000,8000,20000 --mono bin

Calibration (lens covered for the dark):
    python grab.py -e 20000 -g 128 -n 16 --save-dark dark.npy
    python grab.py -e 20000 -g 128 -n 8 --dark dark.npy --mono bin

A dark frame is only valid for the exposure and gain it was taken at.

Diagnostics:
    python grab.py -e 4000 -g 128 --compare        # try each RAW10 layout
    python grab.py -e 4000 -g 128 --test-pattern 1 # sensor test pattern
    python grab.py -e 4000 -g 128 --keep-raw       # save the raw for period.py

Notes:
  The board has 64MB of RAM and /tmp is a RAM disk, so multi-frame captures
  are pulled in batches (see --batch) rather than buffered on the board.

  Exposure and blanking are measured in rows, not seconds. One row is about
  27.45us, so exposure 20000 is roughly 550ms. Exposure cannot exceed the
  frame length, which is 1296 image rows plus the vertical blanking, so the
  script raises blanking automatically when it needs to.

Requires numpy and pillow on this PC, and either adb on PATH or ssh/scp.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import cv2

import numpy as np
from PIL import Image

SUBDEV = "/dev/v4l-subdev2"
VIDEO = "/dev/video0"
REMOTE_FILE = "/tmp/grab.raw"

ROW_TIME_US = 27.45  # 2800 pixel clocks at 102 MP/s

# Bayer offsets within each 2x2 block. Change if red and blue look swapped.
BAYER = {"B": (0, 0), "G1": (0, 1), "G2": (1, 0), "R": (1, 1)}  # BGGR

PACKINGS = ("bits_lsb", "word32", "mipi5")


# ----------------------------------------------------------------- transport

class Board:
    """Runs commands on the board over adb or ssh."""

    def __init__(self, ssh_host=None):
        self.ssh_host = ssh_host

    def run(self, command):
        argv = (["ssh", f"root@{self.ssh_host}", command] if self.ssh_host
                else ["adb", "shell", command])
        result = subprocess.run(argv, capture_output=True, text=True)
        if result.returncode:
            sys.exit(f"command failed: {command}\n{result.stderr.strip()}")
        return result.stdout

    def fetch(self, remote, local):
        argv = (["scp", f"root@{self.ssh_host}:{remote}", local] if self.ssh_host
                else ["adb", "pull", remote, local])
        result = subprocess.run(argv, capture_output=True, text=True)
        if result.returncode:
            sys.exit(f"could not fetch {remote}\n{result.stderr.strip()}")


# ------------------------------------------------------------------ controls

def read_ctrls(board):
    text = board.run(f"v4l2-ctl -d {SUBDEV} --list-ctrls")
    pattern = re.compile(
        r"^\s*(\w+)\s+0x[0-9a-f]+\s+\(\w+\)\s*:\s*min=(-?\d+)\s+max=(-?\d+)"
        r".*?value=(-?\d+)", re.MULTILINE)
    return {m.group(1): {"min": int(m.group(2)), "max": int(m.group(3)),
                         "value": int(m.group(4))}
            for m in pattern.finditer(text)}


def apply_settings(board, args, exposure):
    ctrls = read_ctrls(board)
    vblank = args.vblank

    # Exposure is capped by frame length, so blanking has to be set first.
    if exposure is not None and vblank is None:
        headroom = ctrls.get("exposure", {}).get("max", 0)
        if exposure > headroom:
            vb = ctrls.get("vertical_blanking", {})
            vblank = min(vb.get("value", 0) + (exposure - headroom) + 64,
                         vb.get("max", 0))
            print(f"  raising vertical_blanking to {vblank} "
                  f"to fit exposure {exposure}")

    if vblank is not None:
        board.run(f"v4l2-ctl -d {SUBDEV} -c vertical_blanking={vblank}")
    if args.gain is not None:
        board.run(f"v4l2-ctl -d {SUBDEV} -c analogue_gain={args.gain}")
    if exposure is not None:
        board.run(f"v4l2-ctl -d {SUBDEV} -c exposure={exposure}")
    if args.test_pattern is not None:
        board.run(f"v4l2-ctl -d {SUBDEV} -c test_pattern={args.test_pattern}")
        print(f"  test_pattern       = {args.test_pattern}")

    after = read_ctrls(board)
    for name in ("exposure", "analogue_gain", "vertical_blanking"):
        if name in after:
            c = after[name]
            print(f"  {name:18s} = {c['value']:<8d} (max {c['max']})")

    if "exposure" in after:
        ms = after["exposure"]["value"] * ROW_TIME_US / 1000.0
        print(f"  exposure time      ~ {ms:.1f} ms")


def capture(board, args, local_path, count):
    board.run(f"v4l2-ctl -d {VIDEO} --set-fmt-video="
              f"width={args.width},height={args.height},"
              f"pixelformat={args.pixfmt}")
    board.run(f"v4l2-ctl -d {VIDEO} --stream-mmap --stream-count={count} "
              f"--stream-to={REMOTE_FILE}")
    board.fetch(REMOTE_FILE, local_path)


# ----------------------------------------------------------------- unpacking

def unpack_bits_lsb(rows, width):
    """10 bits per pixel packed continuously, little-endian throughout.

    Bits run least-significant-first within each byte, and each pixel's ten
    bits are assembled least-significant-first. Getting either wrong shuffles
    every pixel's value in a way that looks exactly like noise.
    """
    height = rows.shape[0]
    nbytes = width * 10 // 8
    if nbytes > rows.shape[1]:
        raise ValueError("stride too small for compact 10-bit data")
    bits = np.unpackbits(rows[:, :nbytes], axis=1, bitorder="little")
    bits = bits.reshape(height, width, 10)
    return (bits * (2 ** np.arange(10))).sum(axis=2).astype(np.uint16)


def unpack_word32(rows, width):
    """Three pixels per little-endian 32-bit word, 10 bits each."""
    height = rows.shape[0]
    if width % 3:
        raise ValueError("word32 packing needs a width divisible by 3")
    n = width // 3
    if n * 4 > rows.shape[1]:
        raise ValueError("stride too small for 32-bit words")
    words = np.ascontiguousarray(rows[:, :n * 4]).view(np.uint32)
    out = np.empty((height, n, 3), dtype=np.uint16)
    for i in range(3):
        out[:, :, i] = (words >> (10 * i)) & 0x3FF
    return out.reshape(height, width)


def unpack_mipi5(rows, width):
    """Four pixels per five bytes: four high bytes then a byte of low bits."""
    height = rows.shape[0]
    packed = width * 5 // 4
    if packed > rows.shape[1]:
        raise ValueError("stride too small for 5-byte groups")
    groups = rows[:, :packed].reshape(height, -1, 5)
    high = groups[:, :, :4].astype(np.uint16) << 2
    low = groups[:, :, 4]
    for pixel in range(4):
        high[:, :, pixel] |= (low >> (2 * pixel)) & 0x03
    return high.reshape(height, width)


UNPACKERS = {"bits_lsb": unpack_bits_lsb,
             "word32": unpack_word32,
             "mipi5": unpack_mipi5}


def unpack_frames(path, width, height, count, packing):
    """Return float32 array of shape (count, height, width), values 0 to 1023."""
    data = np.fromfile(path, dtype=np.uint8)
    frame_bytes = data.size // count
    if frame_bytes * count != data.size:
        sys.exit(f"file size {data.size} is not {count} whole frames. "
                 "The capture was probably truncated for lack of space; "
                 "try a smaller --batch.")
    if frame_bytes % height:
        sys.exit(f"frame size {frame_bytes} not divisible by height {height}. "
                 "The capture was probably truncated; try a smaller --batch.")
    stride = frame_bytes // height

    unpacker = UNPACKERS[packing]
    out = np.empty((count, height, width), dtype=np.float32)
    for i in range(count):
        chunk = data[i * frame_bytes:(i + 1) * frame_bytes]
        out[i] = unpacker(chunk.reshape(height, stride), width)
    return out, stride


# ---------------------------------------------------------------- processing

def to_mono(img, mode):
    """Collapse each 2x2 Bayer block into one clean greyscale pixel."""
    if mode == "bin":
        # Every output pixel averages the same set of filters, so the mosaic
        # cancels and the noise drops by about half.
        return (img[0::2, 0::2] + img[0::2, 1::2]
                + img[1::2, 0::2] + img[1::2, 1::2]) * 0.25
    if mode == "green":
        # Green only: sharper, and closest to overall luminance.
        return (img[BAYER["G1"][0]::2, BAYER["G1"][1]::2]
                + img[BAYER["G2"][0]::2, BAYER["G2"][1]::2]) * 0.5
    raise ValueError(f"unknown mono mode {mode}")


def debayer_bin(img):
    """Half-resolution colour from each 2x2 block. No interpolation."""
    def plane(name):
        r, c = BAYER[name]
        return img[r::2, c::2]
    return np.dstack([plane("R"),
                      (plane("G1") + plane("G2")) * 0.5,
                      plane("B")])


def grey_world(rgb):
    """White balance from unclipped pixels only, with highlight clipping fix."""
    flat = rgb.reshape(-1, 3)
    good = flat[flat.max(axis=1) < 900]
    means = (good if len(good) > 1000 else flat).mean(axis=0)
    means[means == 0] = 1.0
    
    # Calculate the multipliers
    multipliers = means.mean() / means
    balanced = rgb * multipliers

    white_clip = 1023.0 * multipliers.min()
    
    return np.clip(balanced, 0, white_clip)


def column_banding(img):
    """Mean step between columns two apart, skipping the Bayer alternation."""
    return float(np.abs(np.diff(img[:, ::2], axis=1)).mean())


def to_png(data, out_path, gamma=True, low=0.2, high=100.0):
    lo = float(np.percentile(data, low))
    hi = float(np.percentile(data, high))
    if hi <= lo:
        lo, hi = float(data.min()), float(data.max())
    if hi <= lo:
        sys.exit("frame is completely flat; the sensor is seeing nothing")
    norm = np.clip((data - lo) / (hi - lo), 0.0, 1.0)
    if gamma:
        norm = np.power(norm, 1.0 / 2.2)
    img_16bit = (norm * 65535).astype(np.uint16)
    if data.ndim == 3:
        img_16bit = img_16bit[:, :, ::-1]
    cv2.imwrite(out_path, img_16bit)
    

        
    "Image.fromarray((norm * 255).astype(np.uint16)).save(out_path)"


def report(single, stacked, count):
    print(f"  range {stacked.min():.0f} to {stacked.max():.0f} of 1023, "
          f"mean {stacked.mean():.1f}")
    if count > 1:
        print(f"  averaged {count} frames "
              f"(expect roughly {np.sqrt(count):.1f}x less random noise)")
        print(f"  one frame spread {single.std():.1f}, "
              f"after averaging {stacked.std():.1f}")
    if stacked.max() >= 1020:
        print("  ! clipping: reduce exposure or gain")
    elif stacked.max() < 300:
        print("  ! very dim: increase exposure")


# --------------------------------------------------------------------- main

def run_compare(board, args, tag):
    """Capture one frame and render it under every candidate layout."""
    with tempfile.TemporaryDirectory() as tmp:
        raw_path = os.path.join(tmp, "grab.raw")
        capture(board, args, raw_path, 1)
        print()
        for packing in PACKINGS:
            try:
                frames, _ = unpack_frames(raw_path, args.width, args.height,
                                          1, packing)
            except (ValueError, SystemExit) as exc:
                print(f"  {packing:10s} not applicable: {exc}")
                continue
            img = frames[0]
            out_png = f"{args.out}{tag}_{packing}.png"
            to_png(img, out_png, not args.linear)
            print(f"  {packing:10s} banding {column_banding(img):7.2f}"
                  f"  ->  {out_png}")
        print("\n  open them and keep the one without banding, then pass\n"
              "  --packing <name> or change the default.")
    return f"{args.out}{tag}_{PACKINGS[0]}.png"


def one_shot(board, args, exposure, tag=""):
    apply_settings(board, args, exposure)

    if args.compare:
        return run_compare(board, args, tag)

    # Capture in batches: /tmp is a RAM disk on a 64MB board, so a full
    # multi-frame capture will not fit there.
    with tempfile.TemporaryDirectory() as tmp:
        raw_path = os.path.join(tmp, "grab.raw")
        total = first = stride = None
        remaining = args.frames
        while remaining:
            batch = min(args.batch, remaining)
            capture(board, args, raw_path, batch)
            frames, stride = unpack_frames(raw_path, args.width, args.height,
                                           batch, args.packing)
            if first is None:
                first = frames[0].copy()
            batch_sum = frames.sum(axis=0)
            total = batch_sum if total is None else total + batch_sum
            remaining -= batch
        stacked = total / args.frames

        if args.keep_raw:
            os.replace(raw_path, f"{args.out}{tag}.raw")
            print(f"  kept raw as {args.out}{tag}.raw (last batch only)")

    print(f"  stride {stride} bytes per line, packing {args.packing}")

    if args.save_dark:
        np.save(args.save_dark, stacked)
        print(f"  saved dark frame to {args.save_dark}")

    if args.dark:
        dark = np.load(args.dark)
        if dark.shape != stacked.shape:
            sys.exit("dark frame size does not match this capture")
        stacked = np.clip(stacked - dark, 0, None)
        first = np.clip(first - dark, 0, None)
        print(f"  subtracted dark frame {args.dark}")

    elif args.black:
        stacked = np.clip(stacked - args.black, 0, None)
        first = np.clip(first - args.black, 0, None)
        print(f"  subtracted black level {args.black}")

    report(first, stacked, args.frames)

    out_png = f"{args.out}{tag}.png"
    if args.mono:
        img = to_mono(stacked, args.mono)
        to_png(img, out_png, not args.linear)
        print(f"  wrote {out_png} (mono {args.mono}, "
              f"{img.shape[1]}x{img.shape[0]})")
    elif args.colour:
        raw_uint16 = np.clip(stacked, 0, 1023).astype(np.uint16)
        full_res_rgb = cv2.cvtColor(raw_uint16, cv2.COLOR_BayerRG2RGB_EA)
        rgb = grey_world(full_res_rgb.astype(np.float32))
        to_png(rgb, out_png, not args.linear)
        print(f"  wrote {out_png} (colour, {rgb.shape[1]}x{rgb.shape[0]})")
        #rgb = grey_world(debayer_bin(stacked))
        #to_png(rgb, out_png, not args.linear)
        #print(f"  wrote {out_png} (colour, {rgb.shape[1]}x{rgb.shape[0]})")
    else:
        to_png(stacked, out_png, not args.linear)
        print(f"  wrote {out_png} (raw Bayer, {args.width}x{args.height})")
    return out_png


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)

    p.add_argument("-e", "--exposure", type=int, help="exposure in rows")
    p.add_argument("-g", "--gain", type=int, help="analogue gain (128 = 1x)")
    p.add_argument("-b", "--vblank", type=int, help="vertical blanking in rows")
    p.add_argument("-n", "--frames", type=int, default=1,
                   help="capture and average this many frames")
    p.add_argument("--batch", type=int, default=1,
                   help="frames per capture before pulling (default 1; the "
                        "board has little RAM and /tmp is a RAM disk)")

    p.add_argument("--mono", choices=("bin", "green"),
                   help="clean half-resolution greyscale")
    p.add_argument("--colour", "--color", action="store_true", dest="colour",
                   help="debayer to half-resolution colour")

    p.add_argument("--dark", metavar="NPY", help="subtract this dark frame")
    p.add_argument("--save-dark", metavar="NPY",
                   help="save this capture as a dark frame")

    p.add_argument("--sweep", help="comma-separated exposures, one frame each")
    p.add_argument("--packing", choices=PACKINGS, default="bits_lsb",
                   help="RAW10 layout (default: bits_lsb)")
    p.add_argument("--compare", action="store_true",
                   help="render one frame under every layout")
    p.add_argument("--test-pattern", type=int, metavar="N",
                   help="sensor test pattern 0-4 (0 disables)")

    p.add_argument("-o", "--out", default="frame", help="output name stem")
    p.add_argument("--width", type=int, default=2304)
    p.add_argument("--height", type=int, default=1296)
    p.add_argument("--pixfmt", default="BG10")
    p.add_argument("--ssh", metavar="HOST", help="use ssh/scp instead of adb")
    p.add_argument("--linear", action="store_true",
                   help="skip the gamma curve in the preview")
    p.add_argument("--keep-raw", action="store_true", help="also save the raw")
    p.add_argument("--no-open", action="store_true")
    p.add_argument("--black", type=int, default=64,
                   help="black level to subtract, in 10-bit counts")

    args = p.parse_args()

    if args.mono and args.colour:
        sys.exit("--mono and --colour are mutually exclusive")

    board = Board(args.ssh)

    if args.sweep:
        result = None
        for value in (int(v) for v in args.sweep.split(",")):
            print(f"\nexposure {value}:")
            result = one_shot(board, args, value, tag=f"_e{value}")
    else:
        result = one_shot(board, args, args.exposure)

    if result and not args.no_open:
        if sys.platform == "win32":
            os.startfile(os.path.abspath(result))
        elif sys.platform.startswith("linux"):
            subprocess.run(["xdg-open", os.path.abspath(result)])


if __name__ == "__main__":
    main()
