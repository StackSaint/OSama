#!/usr/bin/env python3
"""Convert a Bad Apple!! video into the OSama `badapple.dat` format.

The OS player draws every frame in VGA text mode using half-block characters,
so one text cell shows two vertical pixels. The default 58x48 pixel grid fills
58 columns x 24 rows (row 24 is the player status line) and keeps the 4:3
picture close to its real aspect ratio (each pixel is 9x8 screen pixels).

Data file layout (all integers little-endian):

    offset  size  field
    0       4     magic "BAPL"
    4       2     width  in pixels
    6       2     height in pixels
    8       2     frames per second
    10      2     format version (1)
    12      4     frame count

Each frame is a 1-bit picture (row-major, 1 = white) encoded as the XOR
difference with the previous frame (the frame before the first one is all
black). The difference is stored as run lengths that alternate between
"unchanged" and "changed" pixels, starting with "unchanged". Every run length
is a LEB128 varint (7 bits per byte, high bit = more bytes follow). A frame
ends as soon as its runs cover width * height pixels, so no frame length is
stored.

Usage:
    python tools/badapple-convert.py badapple.mp4 other/badapple.dat
    python tools/badapple-convert.py badapple.mp4 out.dat --ffmpeg /path/to/ffmpeg

ffmpeg must be installed (or passed with --ffmpeg). One way to get the video:
    yt-dlp -f "bv*[height<=480]" -o badapple.mp4 https://youtu.be/FtutLA63Cp8
"""

import argparse
import shutil
import struct
import subprocess
import sys

MAGIC = b"BAPL"
VERSION = 1


def encode_varint(value, out):
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return


def encode_frame(previous, current, out):
    """Append the run-length XOR difference between two frames to `out`."""
    changed = False
    run = 0
    for old, new in zip(previous, current):
        if (old != new) == changed:
            run += 1
        else:
            encode_varint(run, out)
            changed = not changed
            run = 1
    encode_varint(run, out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("video", help="input video file (e.g. badapple.mp4)")
    parser.add_argument("output", help="output .dat file")
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg",
                        help="path to the ffmpeg executable")
    parser.add_argument("--width", type=int, default=58)
    parser.add_argument("--height", type=int, default=48)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--threshold", type=int, default=128,
                        help="gray level (0-255) treated as white")
    args = parser.parse_args()

    if args.height % 2 != 0:
        parser.error("height must be even (two pixels per text cell)")

    pixel_count = args.width * args.height
    command = [
        args.ffmpeg, "-v", "error", "-i", args.video,
        "-vf", f"fps={args.fps},scale={args.width}:{args.height}:flags=area,format=gray",
        "-f", "rawvideo", "-",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE)

    body = bytearray()
    previous = bytes(pixel_count)
    frame_count = 0
    while True:
        raw = process.stdout.read(pixel_count)
        if len(raw) < pixel_count:
            break
        current = bytes(1 if value >= args.threshold else 0 for value in raw)
        encode_frame(previous, current, body)
        previous = current
        frame_count += 1

    if process.wait() != 0 or frame_count == 0:
        sys.exit("ffmpeg failed or produced no frames")

    header = MAGIC + struct.pack("<HHHHI", args.width, args.height,
                                 args.fps, VERSION, frame_count)
    with open(args.output, "wb") as output:
        output.write(header)
        output.write(body)

    total = len(header) + len(body)
    raw_size = frame_count * pixel_count // 8
    print(f"{frame_count} frames, {args.width}x{args.height} @ {args.fps} fps")
    print(f"{total} bytes ({raw_size} bytes as raw 1-bit frames)")


if __name__ == "__main__":
    main()
