#!/usr/bin/env python3
"""Capture a screenshot of the device's current LVGL screen over serial.

Sends the "screenshot" console command (registered by
main/dev_screenshot_console.c, only present in dev builds with
CONFIG_DEV_SCREENSHOT_CONSOLE enabled) and decodes the framed base64 reply
into a PNG file.

Pass --nav to first send the "nav" console command (registered by
display_ui_register_dev_nav_console() in main/display_ui.c, same build gate)
so the device jumps to a specific screen before capturing - no physical tap
needed. Run with ``--help`` for the complete target list; it mirrors every
lazy screen registered by ``display_ui_register_dev_nav_console()``.

Wire format (see dev_screenshot_console.c for the firmware side):

    SCREENSHOT_BEGIN width=480 height=800 stride=960 format=RGB565 bytes=768000
    SCREENSHOT_DATA <base64 chunk>
    ...
    SCREENSHOT_END crc32=<hex> bytes=768000

or, on failure:

    SCREENSHOT_ERROR reason=<no_mem|snapshot_failed|lock_failed>
"""

import argparse
import base64
import sys
import time
import zlib

import numpy as np
import serial
from PIL import Image


def parse_kv_line(line, prefix):
    rest = line[len(prefix):].strip()
    fields = {}
    for token in rest.split():
        key, _, value = token.partition("=")
        fields[key] = value
    return fields


def rgb565_to_rgb888(raw, width, height, stride):
    row_bytes = width * 2
    pixels = np.empty((height, width, 3), dtype=np.uint8)
    for row in range(height):
        start = row * stride
        row_raw = raw[start:start + row_bytes]
        px = np.frombuffer(row_raw, dtype="<u2")
        r = (px >> 11) & 0x1F
        g = (px >> 5) & 0x3F
        b = px & 0x1F
        pixels[row, :, 0] = (r.astype(np.uint16) * 255 // 31).astype(np.uint8)
        pixels[row, :, 1] = (g.astype(np.uint16) * 255 // 63).astype(np.uint8)
        pixels[row, :, 2] = (b.astype(np.uint16) * 255 // 31).astype(np.uint8)
    return pixels


def navigate(ser, timeout, nav, nav_ssid):
    command = f"nav {nav}" + (f" {nav_ssid}" if nav_ssid else "") + "\n"
    ser.write(command.encode("ascii"))
    ser.flush()

    deadline = time.time() + timeout
    while time.time() < deadline:
        raw_line = ser.readline()
        if not raw_line:
            continue
        line = raw_line.decode("ascii", errors="replace").strip()
        if line.startswith("NAV_ERROR"):
            fields = parse_kv_line(line, "NAV_ERROR")
            raise RuntimeError(f"device reported nav error: {fields.get('reason', 'unknown')}")
        if line.startswith("NAV_OK"):
            return
    raise TimeoutError("never saw NAV_OK - is CONFIG_DEV_SCREENSHOT_CONSOLE enabled on this build?")


def capture(port, baud, timeout, out_path, nav=None, nav_ssid=None):
    ser = serial.Serial(port, baud, timeout=timeout)
    time.sleep(0.3)
    ser.reset_input_buffer()

    if nav:
        navigate(ser, timeout, nav, nav_ssid)
        time.sleep(0.3)  # let the LVGL flag flips / re-render settle before capturing

    ser.write(b"screenshot\n")
    ser.flush()

    header = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw_line = ser.readline()
        if not raw_line:
            continue
        line = raw_line.decode("ascii", errors="replace").strip()
        if line.startswith("SCREENSHOT_ERROR"):
            fields = parse_kv_line(line, "SCREENSHOT_ERROR")
            raise RuntimeError(f"device reported error: {fields.get('reason', 'unknown')}")
        if line.startswith("SCREENSHOT_BEGIN"):
            header = parse_kv_line(line, "SCREENSHOT_BEGIN")
            break
    if header is None:
        raise TimeoutError("never saw SCREENSHOT_BEGIN - is CONFIG_DEV_SCREENSHOT_CONSOLE enabled on this build?")

    width = int(header["width"])
    height = int(header["height"])
    stride = int(header["stride"])
    expected_bytes = int(header["bytes"])

    raw = bytearray()
    end_fields = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        raw_line = ser.readline()
        if not raw_line:
            continue
        line = raw_line.decode("ascii", errors="replace").strip()
        if line.startswith("SCREENSHOT_DATA "):
            raw.extend(base64.b64decode(line[len("SCREENSHOT_DATA "):]))
        elif line.startswith("SCREENSHOT_END"):
            end_fields = parse_kv_line(line, "SCREENSHOT_END")
            break
    ser.close()

    if end_fields is None:
        raise TimeoutError("never saw SCREENSHOT_END - reply may have been truncated")
    if len(raw) != expected_bytes:
        print(f"warning: got {len(raw)} bytes, expected {expected_bytes}", file=sys.stderr)

    reported_crc = int(end_fields["crc32"], 16)
    actual_crc = zlib.crc32(bytes(raw)) & 0xFFFFFFFF
    if reported_crc != actual_crc:
        print(f"warning: CRC32 mismatch (device={reported_crc:08x}, computed={actual_crc:08x}) - "
              "image may be corrupted", file=sys.stderr)

    pixels = rgb565_to_rgb888(bytes(raw), width, height, stride)
    Image.fromarray(pixels, "RGB").save(out_path)
    return out_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbmodem101")
    parser.add_argument("--out", required=True, help="Output PNG path")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=15.0, help="Read timeout in seconds")
    parser.add_argument(
        "--nav",
        choices=[
            "watchlist", "settings", "wifi", "wifi_password", "watchlist_manage", "watchlist_add", "time",
            "time_format", "date_format", "time_zones", "time_zone_cities", "region", "display", "night_time",
            "updates", "about", "disclaimer",
        ],
        help="Navigate to this screen (via the device's dev-only 'nav' console command) before capturing",
    )
    parser.add_argument("--ssid", help="SSID title to show on the password screen (only used with --nav wifi_password)")
    args = parser.parse_args()

    out_path = capture(args.port, args.baud, args.timeout, args.out, nav=args.nav, nav_ssid=args.ssid)
    print(out_path)


if __name__ == "__main__":
    main()
