#!/usr/bin/env python3
"""Inspect Chromium resources.pak files.

resources.pak lives next to chrome.exe inside the version directory, e.g.:
  D:\\Programs\\chrome_test\\App\\152.0.7967.2\\resources.pak

The file uses PAK v4 or v5:
  - v4: version(4) + num_entries(4) + encoding(1) + entries(6 bytes each)
  - v5: version(4) + encoding(4) + resource_count(2) + alias_count(2) + entries

Each entry is (resource_id: uint16, file_offset: uint32). The entry after the
last resource has resource_id == 0 and marks the end of the table.

Most large entries are GZIP-compressed. The original uncompressed size is
stored in the last 4 bytes before the next entry's offset.
"""

import argparse
import gzip
import struct
import sys
import zlib


def parse_pak(path):
    with open(path, "rb") as f:
        data = f.read()

    version = struct.unpack("<I", data[:4])[0]
    if version == 4:
        num_entries, encoding = struct.unpack("<IB", data[4:9])
        offset = 9
        print(f"PAK v4: entries={num_entries}, encoding={encoding}")
    elif version == 5:
        encoding, resource_count, alias_count = struct.unpack("<IHH", data[4:12])
        num_entries = resource_count
        offset = 12
        print(f"PAK v5: resources={resource_count}, aliases={alias_count}, encoding={encoding}")
    else:
        raise ValueError(f"Unsupported PAK version: {version}")

    entries = []
    for _ in range(num_entries + 1):  # include sentinel
        rid, foff = struct.unpack("<HI", data[offset:offset + 6])
        entries.append((rid, foff))
        offset += 6
    return data, entries


def decompress_entry(data, foff, next_foff):
    size = next_foff - foff
    if size < 10:
        return None
    entry = data[foff:next_foff]
    if entry[0] != 0x1F or entry[1] != 0x8B or entry[2] != 0x08:
        return None

    try:
        return gzip.decompress(entry)
    except Exception:
        pass

    # Some Chromium entries use non-standard GZIP headers; skip them manually
    # and inflate the raw deflate stream.
    try:
        pos = 10
        flags = entry[3]
        if flags & 0x04:  # FEXTRA
            xlen = struct.unpack("<H", entry[pos:pos + 2])[0]
            pos += 2 + xlen
        if flags & 0x08:  # FNAME
            while entry[pos] != 0:
                pos += 1
            pos += 1
        if flags & 0x10:  # FCOMMENT
            while entry[pos] != 0:
                pos += 1
            pos += 1
        if flags & 0x02:  # FHCRC
            pos += 2
        return zlib.decompress(entry[pos:-4], -15)
    except Exception:
        return None


def iter_gzip_entries(data, entries):
    for i in range(len(entries) - 1):
        rid, foff = entries[i]
        next_foff = entries[i + 1][1]
        size = next_foff - foff
        if size < 10240:
            continue
        dec = decompress_entry(data, foff, next_foff)
        if dec is None:
            continue
        yield rid, foff, size, dec


def cmd_search(args):
    data, entries = parse_pak(args.pak)
    patterns = [p.encode("utf-8") for p in args.pattern]
    for rid, foff, size, dec in iter_gzip_entries(data, entries):
        for pat in patterns:
            if pat in dec:
                print(f"\nresource_id={rid}, offset={foff}, raw={size}, decompressed={len(dec)}")
                pos = dec.find(pat)
                snippet = dec[max(0, pos - 100):pos + len(pat) + 100]
                print(snippet.decode("utf-8", errors="replace").replace("\n", " "))


def cmd_dump(args):
    data, entries = parse_pak(args.pak)
    for i, (rid, foff) in enumerate(entries[:-1]):
        if rid == args.resource_id:
            next_foff = entries[i + 1][1]
            dec = decompress_entry(data, foff, next_foff)
            if dec is None:
                dec = data[foff:next_foff]
            out_path = args.output or f"pak_res_{rid}.bin"
            with open(out_path, "wb") as f:
                f.write(dec)
            print(f"Dumped resource {rid} ({len(dec)} bytes) to {out_path}")
            return
    print(f"Resource {args.resource_id} not found")


def main():
    parser = argparse.ArgumentParser(description="Inspect Chromium resources.pak")
    sub = parser.add_subparsers(dest="command", required=True)

    p_search = sub.add_parser("search", help="Search for strings in GZIP entries")
    p_search.add_argument("pak")
    p_search.add_argument("pattern", nargs="+")
    p_search.set_defaults(func=cmd_search)

    p_dump = sub.add_parser("dump", help="Dump a resource by id")
    p_dump.add_argument("pak")
    p_dump.add_argument("resource_id", type=int)
    p_dump.add_argument("-o", "--output")
    p_dump.set_defaults(func=cmd_dump)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
