#!/usr/bin/env python3
"""Compare the harness's pre-present frame with WarmerSun's graded frame.

Reads two uncompressed 32-bit DDS files (the format both writers emit) and
reports luminance, warmth and contrast. Exits non-zero when the grade did not
run or produced an implausible result.
"""
import struct
import sys
import pathlib

DDS_MAGIC = b"DDS "


def read_dds(path):
    data = pathlib.Path(path).read_bytes()
    if data[:4] != DDS_MAGIC:
        raise ValueError(f"{path}: not a DDS file")
    height, width = struct.unpack_from("<II", data, 12)
    pitch = struct.unpack_from("<I", data, 20)[0]
    pixel_format = struct.unpack_from("<I", data, 80)[0]
    bits = struct.unpack_from("<I", data, 88)[0]
    if (pixel_format & 0x40) == 0 or bits != 32:
        raise ValueError(f"{path}: expected uncompressed 32-bit RGBA")
    offset = 128
    rows = []
    for y in range(height):
        row = data[offset + y * pitch: offset + y * pitch + width * 4]
        rows.append(row)
    return width, height, rows


def stats(path):
    width, height, rows = read_dds(path)
    total = width * height
    sr = sg = sb = 0.0
    lumas = []
    clipped = 0
    for row in rows:
        for x in range(width):
            b, g, r, _a = row[x * 4:x * 4 + 4]
            r, g, b = r / 255.0, g / 255.0, b / 255.0
            sr += r
            sg += g
            sb += b
            l = 0.2126 * r + 0.7152 * g + 0.0722 * b
            lumas.append(l)
            if l > 0.985:
                clipped += 1
    mean_l = sum(lumas) / total
    var = sum((l - mean_l) ** 2 for l in lumas) / total
    return {
        "width": width,
        "height": height,
        "r": sr / total,
        "g": sg / total,
        "b": sb / total,
        "luma": mean_l,
        "contrast": var ** 0.5,
        "warmth": (sr / total) / max(sb / total, 1e-6),
        "clip": clipped / total,
    }


def main():
    before_path = sys.argv[1] if len(sys.argv) > 1 else "harness_before.dds"
    after_path = sys.argv[2] if len(sys.argv) > 2 else "WarmerSun_frame_00000.dds"

    before = stats(before_path)
    after = stats(after_path)

    print(f"{'metric':<10}{'before':>10}{'after':>10}{'delta':>10}")
    for key in ("r", "g", "b", "luma", "contrast", "warmth", "clip"):
        print(f"{key:<10}{before[key]:>10.4f}{after[key]:>10.4f}{after[key] - before[key]:>+10.4f}")

    failures = []
    if (before["width"], before["height"]) != (after["width"], after["height"]):
        failures.append("dimension mismatch")
    if abs(after["luma"] - before["luma"]) > 0.20:
        failures.append("luminance moved too far (grade not calibrated)")
    if after["warmth"] <= before["warmth"] + 0.005:
        failures.append("image did not get warmer")
    if after["contrast"] <= before["contrast"]:
        failures.append("contrast did not increase")
    if after["clip"] > before["clip"] + 0.02:
        failures.append("highlight clipping increased")

    if failures:
        print("\nFAIL:")
        for f in failures:
            print("  -", f)
        return 1

    print("\nPASS: WarmerSun grade applied (warmer, more contrast, no extra clipping)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
