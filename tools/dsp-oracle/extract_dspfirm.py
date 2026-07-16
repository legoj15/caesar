#!/usr/bin/env python3
"""Extract + self-certify the 3DS DSP1 (Teak) firmware from title code.bin dumps.

Each sound-capable 3DS title embeds a full DSP1 firmware image inside its ARM11
code.bin. This tool slices that image at the known per-title file offset, VERIFIES
every segment's embedded SHA-256 (self-certifying: correct segment hashes prove the
slice offset is right), writes a standalone <Title>_dspfirm.cdc, and prints a table.

The firmware is Nintendo copyright — the emitted firmware/ dir is gitignored and
must NEVER be committed. This tool only regenerates it locally from dumps you own.

Adapted from the stage-3 recon scratch extractor (self-certifying original).

Usage:
    python extract_dspfirm.py [--dumps-root DIR] [--out DIR]

Exit code is non-zero if ANY segment fails verification.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

MEMTYPE = {0: "PROG(code)", 1: "@0x1FF00000", 2: "DATA"}

# File offset of the DSP1 blob (its RSA signature start) inside each title's
# already-decompressed exefs/code.bin.
APPS = {
    "MiiPlaza":       0x255100,
    "eShop":          0x2E1CD0,
    "Photos_Camera":  0x341548,
    "Sound_SNOTE":    0x23D1F4,
    "SystemSettings": 0x19A1C0,
}

DEFAULT_DUMPS_ROOT = r"E:\legoj\Documents\3DSWii Dumps\re_extract"


def parse(blob: bytes, name: str, out: Path) -> tuple[bool, dict]:
    """Slice + verify one DSP1 image out of a code.bin tail. Returns (ok, info)."""
    if blob[0x100:0x104] != b"DSP1":
        print(f"=== {name} ===")
        print(f"  ERROR: bad magic {blob[0x100:0x104]!r} at +0x100 — wrong offset?")
        return False, {}

    size = struct.unpack_from("<I", blob, 0x104)[0]
    layout = struct.unpack_from("<H", blob, 0x108)[0]
    special_type = blob[0x10D]
    nseg = blob[0x10E]
    flags = blob[0x10F]
    special_addr, special_size = struct.unpack_from("<II", blob, 0x110)
    fw = blob[:size]
    fw_sha = hashlib.sha256(fw).hexdigest()

    print(f"=== {name} ===")
    print(f"  size 0x{size:X} ({size})  layout 0x{layout:04X}  flags 0x{flags:02X}"
          f"  segments {nseg}")
    print(f"  special: word 0x{special_addr:X} size 0x{special_size:X} memtype {special_type}")
    print(f"  sha256(firmware) {fw_sha}")

    all_ok = True
    for i in range(nseg):
        rec = 0x120 + i * 0x30
        off, addr, seg_size = struct.unpack_from("<III", blob, rec)
        mtype = blob[rec + 0x0F]
        want = blob[rec + 0x10: rec + 0x30]
        data = blob[off: off + seg_size]
        ok = hashlib.sha256(data).digest() == want
        all_ok &= ok
        print(f"  seg{i} {MEMTYPE.get(mtype, str(mtype)):<12} fw_off 0x{off:<8X}"
              f" teak_word 0x{addr:<6X} size 0x{seg_size:<6X}"
              f" sha {'OK' if ok else 'MISMATCH'}  {want[:8].hex()}")

    print(f"  => segment hashes {'VERIFIED' if all_ok else 'FAILED'}")
    if all_ok:
        out.mkdir(parents=True, exist_ok=True)
        (out / f"{name}_dspfirm.cdc").write_bytes(fw)
        print(f"  wrote {name}_dspfirm.cdc ({size} bytes)")
    else:
        print(f"  NOT written (verification failed)")
    print()
    return all_ok, {"size": size, "sha256": fw_sha}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dumps-root", default=DEFAULT_DUMPS_ROOT,
                    help="root holding <Title>/exefs/code.bin (default: %(default)s)")
    ap.add_argument("--out", default=None,
                    help="output dir for *_dspfirm.cdc (default: firmware/ next to this script)")
    args = ap.parse_args()

    dumps_root = Path(args.dumps_root)
    out = Path(args.out) if args.out else (Path(__file__).resolve().parent / "firmware")

    print(f"dumps-root: {dumps_root}")
    print(f"out:        {out}\n")

    all_ok = True
    found_any = False
    for name, off in APPS.items():
        code_path = dumps_root / name / "exefs" / "code.bin"
        if not code_path.is_file():
            print(f"=== {name} ===")
            print(f"  SKIP: {code_path} not found\n")
            continue
        found_any = True
        code = code_path.read_bytes()
        ok, _info = parse(code[off:], name, out)
        all_ok &= ok

    if not found_any:
        print("ERROR: no code.bin found under dumps-root — nothing extracted.")
        return 2

    print(f"Result: {'ALL VERIFIED' if all_ok else 'SOME FAILED'} — output in {out}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
