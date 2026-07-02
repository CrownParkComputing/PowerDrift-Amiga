#!/usr/bin/env python3
"""Extract Power Drift (MAME `pdrift`) ROM regions.

This mirrors the ROM_LOAD layout in MAME's sega/segaybd.cpp for Power Drift.
Supported sets: pdrift, pdrifta, pdrifte, pdriftj, pdriftjb.

Outputs are flat regions intended for the first host harness:

  maincpu.bin      0x080000  68000 main, ROM_LOAD16_BYTE interleaved
  subx.bin         0x040000  68000 sub-X, ROM_LOAD16_BYTE interleaved
  suby.bin         0x040000  68000 sub-Y, ROM_LOAD16_BYTE interleaved
  drive_board.bin  0x008000  drive board ROM, kept for reference
  bsprites.bin     0x080000  System16B sprite ROM_REGION16_BE
  ysprites.bin     0x400000  Y-board sprite ROM_REGION64_BE
  ysprites_pix.bin 0x800000  Y-board sprite pixels unpacked to 16 pixels/u64 word
  soundcpu.bin     0x010000  Z80 sound ROM
  pcm.bin          0x200000  SegaPCM samples, including MAME ROM_RELOAD mirrors
  user1.bin        0x080000  split daughter-card PCM reference ROMs concatenated

Usage:
  pdrift_extract_roms.py [rom_zip_or_dir] [set] [output_dir]
  pdrift_extract_roms.py [set]

The script accepts either a pdrift.zip path/directory as argv[1], or searches:
  ../roms/pdrift.zip, ../roms, /home/jon/.mame/roms/pdrift.zip,
  /home/jon/roms/pdrift.zip, /home/jon/Roms/pdrift.zip
"""
from __future__ import annotations

import os
import sys
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
OUT_BASE = ROOT / "source" / "build"


EXPECT = {
    "epr-12017.25": (0x20000, 0x31190322),
    "epr-12016.24": (0x20000, 0x499F64A6),
    "epr-11748.27": (0x20000, 0x82A76CAB),
    "epr-11747.26": (0x20000, 0x9796ECE5),
    "epr-11905.81": (0x20000, 0x1CF68109),
    "epr-11904.80": (0x20000, 0xBB993681),
    "epr-12019a.54": (0x20000, 0x11188A30),
    "epr-12018a.53": (0x20000, 0x1C582E1F),
    "epr-11485.ic27": (0x08000, 0x069B4201),
    "epr-11789.16": (0x20000, 0xB86F8D2B),
    "epr-11791.14": (0x20000, 0x36B2910A),
    "epr-11790.17": (0x20000, 0x2A564E66),
    "epr-11792.15": (0x20000, 0xC85CAF6E),
    "epr-11757.67": (0x20000, 0xE46DC478),
    "epr-11758.75": (0x20000, 0x5B435C87),
    "epr-11773.63": (0x20000, 0x1B5D5758),
    "epr-11774.71": (0x20000, 0x2CA0C170),
    "epr-11759.86": (0x20000, 0xAC8111F6),
    "epr-11760.114": (0x20000, 0x91282AF9),
    "epr-11775.82": (0x20000, 0x48225793),
    "epr-11776.110": (0x20000, 0x78C46198),
    "epr-11761.66": (0x20000, 0xBAA5D065),
    "epr-11762.74": (0x20000, 0x1D1AF7A5),
    "epr-11777.62": (0x20000, 0x9662DD32),
    "epr-11778.70": (0x20000, 0x2DFB7494),
    "epr-11763.85": (0x20000, 0x1EE23407),
    "epr-11764.113": (0x20000, 0xE859305E),
    "epr-11779.81": (0x20000, 0xA49CD793),
    "epr-11780.109": (0x20000, 0xD514ED81),
    "epr-11765.65": (0x20000, 0x649E2DFF),
    "epr-11766.73": (0x20000, 0xD92FB7FC),
    "epr-11781.61": (0x20000, 0x9692D4CD),
    "epr-11782.69": (0x20000, 0xC913BB43),
    "epr-11767.84": (0x20000, 0x1F8AD054),
    "epr-11768.112": (0x20000, 0xDB2C4053),
    "epr-11783.80": (0x20000, 0x6D189007),
    "epr-11784.108": (0x20000, 0x57F5FD64),
    "epr-11769.64": (0x20000, 0x28F0AB51),
    "epr-11770.72": (0x20000, 0xD7557EA9),
    "epr-11785.60": (0x20000, 0xE6EF32C4),
    "epr-11786.68": (0x20000, 0x2066B49D),
    "epr-11771.83": (0x20000, 0x67635618),
    "epr-11772.111": (0x20000, 0x0F798D3A),
    "epr-11787.79": (0x20000, 0xE631DC12),
    "epr-11788.107": (0x20000, 0x8464C66E),
    "epr-11899.102": (0x10000, 0xED9FA889),
    "mpr-11754.107": (0x80000, 0xEBEB8484),
    "epr-11756.106": (0x20000, 0x12E43F8A),
    "epr-11755.105": (0x20000, 0xC2DB1244),
    "epr-11895.ic1": (0x20000, 0xEE99A6FD),
    "epr-11896.ic2": (0x20000, 0x4BEBC015),
    "epr-11897.ic3": (0x20000, 0x4463CB95),
    "epr-11898.ic4": (0x20000, 0x5D19D767),
}


SET_ALIASES = {
    "pdrift": {},
    "pdrifta": {
        "epr-12019a.54": ("pdrifta/epr-12019.54", 0x20000, 0xE514D7B6),
        "epr-12018a.53": ("pdrifta/epr-12018.53", 0x20000, 0x0A3F7FAF),
    },
    "pdrifte": {
        "epr-12017.25": ("pdrifte/epr-11901.25", 0x20000, 0x16744BE8),
        "epr-12016.24": ("pdrifte/epr-11900.24", 0x20000, 0x0A170D06),
        "epr-12019a.54": ("pdrifte/epr-11903.54", 0x20000, 0xD004F411),
        "epr-12018a.53": ("pdrifte/epr-11902.53", 0x20000, 0xE8028E08),
    },
    "pdriftj": {
        "epr-12017.25": ("pdriftj/epr-11746a.25", 0x20000, 0xB0F1CAF4),
        "epr-12016.24": ("pdriftj/epr-11745a.24", 0x20000, 0xA89720CD),
        "epr-11905.81": ("pdriftj/epr-11752.81", 0x20000, 0xB6BB8111),
        "epr-11904.80": ("pdriftj/epr-11751.80", 0x20000, 0x7F0D0311),
        "epr-12019a.54": ("pdriftj/epr-11750c.54", 0x20000, 0x4E0F4FCB),
        "epr-12018a.53": ("pdriftj/epr-11749c.53", 0x20000, 0x603921F7),
        "epr-11899.102": ("pdriftj/epr-11753.ic102", 0x10000, 0xE81F5748),
    },
    "pdriftjb": {
        "epr-12017.25": ("pdriftj/epr-11746a.25", 0x20000, 0xB0F1CAF4),
        "epr-12016.24": ("pdriftj/epr-11745a.24", 0x20000, 0xA89720CD),
        "epr-11905.81": ("pdriftj/epr-11752.81", 0x20000, 0xB6BB8111),
        "epr-11904.80": ("pdriftj/epr-11751.80", 0x20000, 0x7F0D0311),
        "epr-12019a.54": ("pdriftjb/epr-11750b.54", 0x20000, 0xBC14CE30),
        "epr-12018a.53": ("pdriftjb/epr-11749b.53", 0x20000, 0x9E385568),
        "epr-11899.102": ("pdriftj/epr-11753.ic102", 0x10000, 0xE81F5748),
    },
}


JAPAN_PCM = {
    "pdriftj": "pdriftj",
    "pdriftjb": "pdriftj",
}


JAPAN_PCM_EXPECT = {
    "epr-11894.ic107": (0x40000, 0xB1E573F2),
    "epr-11893.ic106": (0x40000, 0x58B40F19),
    "epr-11892.ic105": (0x40000, 0x3248A758),
}


YSPRITES = [
    ("epr-11757.67", 0x000000),
    ("epr-11758.75", 0x000001),
    ("epr-11773.63", 0x000002),
    ("epr-11774.71", 0x000003),
    ("epr-11759.86", 0x000004),
    ("epr-11760.114", 0x000005),
    ("epr-11775.82", 0x000006),
    ("epr-11776.110", 0x000007),
    ("epr-11761.66", 0x100000),
    ("epr-11762.74", 0x100001),
    ("epr-11777.62", 0x100002),
    ("epr-11778.70", 0x100003),
    ("epr-11763.85", 0x100004),
    ("epr-11764.113", 0x100005),
    ("epr-11779.81", 0x100006),
    ("epr-11780.109", 0x100007),
    ("epr-11765.65", 0x200000),
    ("epr-11766.73", 0x200001),
    ("epr-11781.61", 0x200002),
    ("epr-11782.69", 0x200003),
    ("epr-11767.84", 0x200004),
    ("epr-11768.112", 0x200005),
    ("epr-11783.80", 0x200006),
    ("epr-11784.108", 0x200007),
    ("epr-11769.64", 0x300000),
    ("epr-11770.72", 0x300001),
    ("epr-11785.60", 0x300002),
    ("epr-11786.68", 0x300003),
    ("epr-11771.83", 0x300004),
    ("epr-11772.111", 0x300005),
    ("epr-11787.79", 0x300006),
    ("epr-11788.107", 0x300007),
]


def set_names() -> str:
    return ", ".join(sorted(SET_ALIASES))


def rom_spec(set_name: str, logical_name: str) -> tuple[str, int, int]:
    source_name, size, crc = SET_ALIASES[set_name].get(
        logical_name,
        (logical_name, EXPECT[logical_name][0], EXPECT[logical_name][1]),
    )
    return source_name, size, crc


def required_entries(set_name: str) -> set[str]:
    entries = {rom_spec(set_name, logical_name)[0] for logical_name in EXPECT}
    if set_name in JAPAN_PCM:
        prefix = JAPAN_PCM[set_name]
        entries.update("%s/%s" % (prefix, name) for name in JAPAN_PCM_EXPECT)
    return entries


def candidates(arg: str | None) -> list[Path]:
    out: list[Path] = []
    if arg:
        out.append(Path(arg).expanduser())
    out.extend(
        [
            ROOT / "roms" / "pdrift.zip",
            ROOT / "roms",
            Path("/home/jon/.mame/roms/pdrift.zip"),
            Path("/home/jon/roms/pdrift.zip"),
            Path("/home/jon/Roms/pdrift.zip"),
            Path("/home/jon/Downloads/pdrift.zip"),
        ]
    )
    return out


class RomSource:
    def __init__(self, path: Path):
        self.path = path
        self.zf: zipfile.ZipFile | None = None
        self.zip_names: set[str] = set()
        if path.is_file():
            self.zf = zipfile.ZipFile(path)
            self.zip_names = set(self.zf.namelist())

    def close(self) -> None:
        if self.zf:
            self.zf.close()

    def has(self, name: str) -> bool:
        if self.zf:
            return name in self.zip_names
        return (self.path / name).is_file()

    def read_entry(self, source_name: str, want_size: int, want_crc: int, label: str | None = None) -> bytes:
        if self.zf:
            with self.zf.open(source_name) as f:
                data = f.read()
        else:
            data = (self.path / source_name).read_bytes()

        got_crc = zlib.crc32(data) & 0xFFFFFFFF
        if len(data) != want_size or got_crc != want_crc:
            raise SystemExit(
                "bad ROM %-16s got %d/%08x want %d/%08x"
                % (source_name, len(data), got_crc, want_size, want_crc)
            )
        if label and label != source_name:
            name = "%s -> %s" % (label, source_name)
        else:
            name = source_name
        print("  [OK] %-36s %7dB crc %08x" % (name, len(data), got_crc))
        return data

    def read(self, set_name: str, logical_name: str) -> bytes:
        source_name, want_size, want_crc = rom_spec(set_name, logical_name)
        return self.read_entry(source_name, want_size, want_crc, logical_name)


def open_source(arg: str | None, set_name: str) -> RomSource:
    missing = []
    required = required_entries(set_name)
    for path in candidates(arg):
        if path.is_file():
            src = RomSource(path)
            if all(src.has(name) for name in required):
                print("using ROM source: %s" % path)
                return src
            src.close()
        if path.is_dir():
            src = RomSource(path)
            if all(src.has(name) for name in required):
                print("using ROM source: %s" % path)
                return src
        missing.append(str(path))
    raise SystemExit("missing %s ROM source; checked:\n  %s" % (set_name, "\n  ".join(missing)))


def load16_byte(*roms: bytes) -> bytes:
    assert len(roms) % 2 == 0
    out = bytearray(sum(len(r) for r in roms))
    for pair in range(len(roms) // 2):
        even = roms[pair * 2]
        odd = roms[pair * 2 + 1]
        assert len(even) == len(odd)
        base = pair * len(even) * 2
        out[base : base + len(even) * 2 : 2] = even
        out[base + 1 : base + len(odd) * 2 : 2] = odd
    return bytes(out)


def load64_byte(roms: list[tuple[bytes, int]], size: int) -> bytes:
    out = bytearray(size)
    for data, offset in roms:
        out[offset : offset + len(data) * 8 : 8] = data
    return bytes(out)


def unpack_ysprite_pixels(region: bytes) -> bytes:
    """Unpack each 64-bit BE sprite word to 16 4-bit pixels, MS nibble first."""
    if len(region) % 8:
        raise SystemExit("ysprite region is not 64-bit aligned")
    out = bytearray((len(region) // 8) * 16)
    pos = 0
    for base in range(0, len(region), 8):
        word = int.from_bytes(region[base : base + 8], "big")
        for shift in range(60, -1, -4):
            out[pos] = (word >> shift) & 0xF
            pos += 1
    return bytes(out)


def write_blob(out_dir: Path, name: str, data: bytes, expect_size: int) -> None:
    if len(data) != expect_size:
        raise SystemExit("%s size %d, expected %d" % (name, len(data), expect_size))
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / name
    path.write_bytes(data)
    print("  wrote %-16s %8dB crc %08x" % (name, len(data), zlib.crc32(data) & 0xFFFFFFFF))


def parse_args(argv: list[str]) -> tuple[str | None, str, Path]:
    source_arg: str | None = None
    set_name = os.environ.get("PDRIFT_SET", "pdrift")
    out_dir_arg: str | None = None

    if len(argv) > 1:
        if argv[1] in SET_ALIASES:
            set_name = argv[1]
        else:
            source_arg = argv[1]
    if len(argv) > 2:
        set_name = argv[2]
    if len(argv) > 3:
        out_dir_arg = argv[3]

    if set_name not in SET_ALIASES:
        raise SystemExit("unknown set %r; supported: %s" % (set_name, set_names()))

    out_dir = Path(out_dir_arg).expanduser() if out_dir_arg else OUT_BASE / set_name
    if not out_dir.is_absolute():
        out_dir = Path.cwd() / out_dir
    return source_arg, set_name, out_dir


def main(argv: list[str]) -> int:
    source_arg, set_name, out_dir = parse_args(argv)
    print("extracting set: %s" % set_name)
    src = open_source(source_arg, set_name)
    try:
        blobs = {name: src.read(set_name, name) for name in EXPECT}
        japan_pcm = {}
        if set_name in JAPAN_PCM:
            prefix = JAPAN_PCM[set_name]
            japan_pcm = {
                name: src.read_entry("%s/%s" % (prefix, name), size, crc, name)
                for name, (size, crc) in JAPAN_PCM_EXPECT.items()
            }
    finally:
        src.close()

    write_blob(
        out_dir,
        "maincpu.bin",
        load16_byte(
            blobs["epr-12017.25"],
            blobs["epr-12016.24"],
            blobs["epr-11748.27"],
            blobs["epr-11747.26"],
        ),
        0x080000,
    )
    write_blob(out_dir, "subx.bin", load16_byte(blobs["epr-11905.81"], blobs["epr-11904.80"]), 0x040000)
    write_blob(out_dir, "suby.bin", load16_byte(blobs["epr-12019a.54"], blobs["epr-12018a.53"]), 0x040000)
    write_blob(out_dir, "drive_board.bin", blobs["epr-11485.ic27"], 0x008000)
    write_blob(
        out_dir,
        "bsprites.bin",
        load16_byte(
            blobs["epr-11789.16"],
            blobs["epr-11791.14"],
            blobs["epr-11790.17"],
            blobs["epr-11792.15"],
        ),
        0x080000,
    )

    ysprites = load64_byte([(blobs[name], offset) for name, offset in YSPRITES], 0x400000)
    write_blob(out_dir, "ysprites.bin", ysprites, 0x400000)
    write_blob(out_dir, "ysprites_pix.bin", unpack_ysprite_pixels(ysprites), 0x800000)

    write_blob(out_dir, "soundcpu.bin", blobs["epr-11899.102"], 0x010000)

    pcm = bytearray([0xFF] * 0x200000)
    if japan_pcm:
        for off in (0x000000, 0x040000):
            pcm[off : off + 0x40000] = japan_pcm["epr-11894.ic107"]
        for off in (0x080000, 0x0C0000):
            pcm[off : off + 0x40000] = japan_pcm["epr-11893.ic106"]
        for off in (0x100000, 0x140000):
            pcm[off : off + 0x40000] = japan_pcm["epr-11892.ic105"]
    else:
        pcm[0x000000:0x080000] = blobs["mpr-11754.107"]
        for off in (0x080000, 0x0A0000, 0x0C0000, 0x0E0000):
            pcm[off : off + 0x20000] = blobs["epr-11756.106"]
        for off in (0x100000, 0x120000, 0x140000, 0x160000):
            pcm[off : off + 0x20000] = blobs["epr-11755.105"]
    write_blob(out_dir, "pcm.bin", bytes(pcm), 0x200000)

    write_blob(
        out_dir,
        "user1.bin",
        blobs["epr-11895.ic1"] + blobs["epr-11896.ic2"] + blobs["epr-11897.ic3"] + blobs["epr-11898.ic4"],
        0x080000,
    )
    print("%s ROM extraction OK -> %s" % (set_name, out_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
