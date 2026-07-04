#!/usr/bin/env python3
"""Read a Sleep-Tracker raw-PPG log (the .bin written by components/sd_logger).

During a recording session the firmware buffers raw MAX3010x samples in PSRAM and
flushes them to
    /sdcard/sleeptrk/YYYYMMDD_HHMMSS_ppg.bin
one block per PPG duty-cycle window (~45 s every 5 min), so the SD card sees a few
large sequential writes instead of a stream of tiny ones.

Binary layout (all integers little-endian):
    file header (16 B):  'PPG1' | version u16 | sample_bytes u16 | t_start u32 | rsv u32
    then repeated blocks:
        block header (12 B):  'PR' | rate_hz u16 | t_unix u32 | n_samples u32
        n_samples x 6 B:      red[3] | ir[3]      (each a 3-byte 18-bit unsigned value)

Sample i in a block is captured at t = t_unix + i / rate_hz. Blocks are separated
by the ~5 min PPG-off gaps; there is no data between them. A power cut mid-flush
leaves at most one torn trailing block, which this reader skips.

    python read_raw_ppg.py 20260702_233000_ppg.bin              # summary
    python read_raw_ppg.py 20260702_233000_ppg.bin --csv out.csv

Time note: t_unix is the RTC wall clock treated as UTC (matching read_night.py) —
rendered as naive wall-clock time, no timezone conversion.
"""
import argparse
import struct
import sys
from datetime import datetime, timezone

FILE_MAGIC  = b"PPG1"
BLOCK_MAGIC = b"PR"


def u24(b):
    """Decode a 3-byte little-endian unsigned int."""
    return b[0] | (b[1] << 8) | (b[2] << 16)


def read_blocks(path):
    """Yield (rate_hz, t_unix, samples) per block; samples is a list of (red, ir)."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 16 or data[:4] != FILE_MAGIC:
        sys.exit(f"{path}: not a raw-PPG file (bad magic {data[:4]!r})")
    version, sample_bytes, t_start, _rsv = struct.unpack_from("<HHII", data, 4)
    if sample_bytes != 6:
        sys.exit(f"{path}: unexpected sample size {sample_bytes} (expected 6)")

    off, blocks, torn = 16, 0, 0
    while off + 12 <= len(data):
        if data[off:off + 2] != BLOCK_MAGIC:
            torn += 1
            break
        rate, t_unix, n = struct.unpack_from("<HII", data, off + 2)
        off += 12
        need = n * 6
        if off + need > len(data):        # torn trailing block (power cut mid-flush)
            torn += 1
            break
        samples = [(u24(data[off + i * 6:off + i * 6 + 3]),
                    u24(data[off + i * 6 + 3:off + i * 6 + 6])) for i in range(n)]
        off += need
        blocks += 1
        yield rate, t_unix, samples

    read_blocks.meta = dict(version=version, t_start=t_start, blocks=blocks, torn=torn)


def ts(t):
    return datetime.fromtimestamp(t, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def main():
    ap = argparse.ArgumentParser(description="Read a Sleep-Tracker raw-PPG .bin log")
    ap.add_argument("path", help="the *_ppg.bin file")
    ap.add_argument("--csv", metavar="OUT",
                    help="write t_unix,red,ir rows (one per sample) to OUT")
    args = ap.parse_args()

    total = 0
    first_t = last_t = None
    out = open(args.csv, "w", newline="") if args.csv else None
    if out:
        out.write("t_unix,red,ir\n")

    print(f"window  {'start':<19}  rate   samples   seconds")
    for i, (rate, t_unix, samples) in enumerate(read_blocks(args.path)):
        n = len(samples)
        total += n
        first_t = t_unix if first_t is None else first_t
        last_t = t_unix + (n / rate if rate else 0)
        print(f"{i:>6}  {ts(t_unix):<19}  {rate:>4}  {n:>8}  {n / rate if rate else 0:>7.1f}")
        if out:
            for j, (red, ir) in enumerate(samples):
                out.write(f"{t_unix + (j / rate if rate else 0):.4f},{red},{ir}\n")

    if out:
        out.close()

    m = getattr(read_blocks, "meta", {})
    span = (last_t - first_t) if (first_t is not None and last_t is not None) else 0
    print(f"\n{m.get('blocks', 0)} block(s), {total} samples, "
          f"session start {ts(m.get('t_start', 0))}, span ~{span / 60:.1f} min")
    if m.get("torn"):
        print("note: a torn trailing block was skipped (power cut mid-flush) - expected, not data loss")
    if out is not None or args.csv:
        print(f"wrote {args.csv}")


if __name__ == "__main__":
    main()
