#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2025 Tenstorrent AI ULC

"""
Add a second signature to an MCUboot image that is already signed.

Both signatures must use the same algorithm (this MCUboot build accepts one
signature type). The protected area, header and image are taken from the first
signed image, so both signatures cover the same hash. The second signer's
KEYHASH + signature TLVs are appended to the unprotected TLV area.

Each signature is verified against its key before the output is written.
"""

import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

TLV_INFO_MAGIC = 0x6907
TLV_KEYHASH = 0x01
# Signature TLVs copied from the second signer; KEYHASH pairs with each one.
SIG_TLVS = {0x20}  # RSA2048_PSS only; the ECDSA and Ed25519 types are not handled


# 452 KiB DMFW slot, the size imgtool is given for the DM app build
DMFW_SLOT_SIZE = 452 * 1024


def read_kconfig(path: Path) -> dict:
    """Parse the simple KEY=VALUE lines of a Zephyr .config file."""
    values = {}
    for line in path.read_text().splitlines():
        if "=" in line and not line.startswith("#"):
            key, _, value = line.partition("=")
            values[key] = value.strip('"')
    return values


def split_image(img: bytes):
    """Return (covered, tlvs, trailing) for a signed MCUboot image."""
    hdr_size, prot_size = struct.unpack_from("<HH", img, 8)
    (img_size,) = struct.unpack_from("<I", img, 12)
    covered = hdr_size + img_size + prot_size

    magic, tot = struct.unpack_from("<HH", img, covered)
    if magic != TLV_INFO_MAGIC:
        raise ValueError(f"unprotected TLV info magic 0x{magic:04x} not found at 0x{covered:x}")
    end = covered + tot

    tlvs = []
    off = covered + 4
    while off < end:
        tlv_type, tlv_len = struct.unpack_from("<HH", img, off)
        tlvs.append((tlv_type, img[off + 4 : off + 4 + tlv_len]))
        off += 4 + tlv_len
    return img[:covered], tlvs, img[end:]


def tlv_bytes(tlvs):
    body = b"".join(struct.pack("<HH", t, len(v)) + v for t, v in tlvs)
    total = 4 + len(body)
    return struct.pack("<HH", TLV_INFO_MAGIC, total) + body


def count_valid_sigs(covered: bytes, tlvs, key_path: Path) -> int:
    """How many signature TLVs verify against this private key's public half."""
    pub = serialization.load_pem_private_key(key_path.read_bytes(), password=None).public_key()
    pss = padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=padding.PSS.AUTO)
    valid = 0
    for tlv_type, value in tlvs:
        if tlv_type != 0x20:  # only RSA2048_PSS is supported here
            continue
        try:
            pub.verify(value, covered, pss, hashes.SHA256())
            valid += 1
        except InvalidSignature:
            pass
    return valid


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--imgtool", required=True, type=Path, help="path to mcuboot scripts/imgtool.py")
    ap.add_argument("--unsigned", required=True, type=Path, help="unsigned zephyr.bin")
    ap.add_argument("--signed", required=True, type=Path, help="image already signed with the first key")
    ap.add_argument("--first-key", required=True, type=Path, help="PEM key that signed --signed")
    ap.add_argument("--second-key", required=True, type=Path, help="PEM key to add")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument(
        "--dmc-config",
        required=True,
        type=Path,
        help="zephyr .config of the DM app build; gives the imgtool version and alignment",
    )
    ap.add_argument(
        "--slot-size",
        type=int,
        default=DMFW_SLOT_SIZE,
        help="image slot size passed to imgtool (default: the 452 KiB DMFW slot)",
    )
    args = ap.parse_args()

    cfg = read_kconfig(args.dmc_config)
    version = cfg["CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION"]
    align = cfg.get("CONFIG_MCUBOOT_BOOT_MAX_ALIGN", "8")
    header_size = struct.unpack_from("<H", args.signed.read_bytes(), 8)[0]

    tmp_second = args.output.with_suffix(".second.bin")
    tmp_second.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            sys.executable,
            str(args.imgtool),
            "sign",
            "--version", version,
            "--header-size", hex(header_size),
            "--slot-size", str(args.slot_size),
            "--align", align,
            "--key", str(args.second_key),
            str(args.unsigned),
            str(tmp_second),
        ],
        check=True,
    )

    first = args.signed.read_bytes()
    second = tmp_second.read_bytes()
    cov1, tlvs1, _ = split_image(first)
    cov2, tlvs2, _ = split_image(second)

    if cov1 != cov2:
        print("error: protected area differs between the two signed images, cannot merge", file=sys.stderr)
        return 1

    # Keep the first signer's TLVs, then add the second signer's KEYHASH and signature
    merged = list(tlvs1)
    for tlv_type, value in tlvs2:
        if tlv_type == TLV_KEYHASH or tlv_type in SIG_TLVS:
            merged.append((tlv_type, value))

    out = cov1 + tlv_bytes(merged)

    # Both keys must verify against the same covered bytes before we write anything
    if count_valid_sigs(cov1, merged, args.first_key) < 1:
        print("error: first key does not verify the merged image", file=sys.stderr)
        return 1
    if count_valid_sigs(cov1, merged, args.second_key) < 1:
        print("error: second key does not verify the merged image", file=sys.stderr)
        return 1

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    tmp_second.unlink(missing_ok=True)
    print(f"wrote {args.output} ({len(out)} bytes), signed by both keys")
    return 0


if __name__ == "__main__":
    sys.exit(main())
