#!/usr/bin/env python3
"""Turns a raw /Logbook/byId/<id>/Descriptors reply into the JSON map that
generate_sbem_tables.py consumes.

    python3 tools/parse_descriptors.py descriptors-1767115789.bin out.json

The Race's map (docs/sbem-descriptor-map.json) was produced by hand once and
the code that did it was not kept, which became a problem the moment a
second watch model turned up with an entirely different numbering. This
script is that step, written down.

Format, from the real replies and the chunk rules decompiled out of
libmds.so's BSML::SmlStreamParser::parseChunkHeader:

    SBEM0103                       magic
    then repeating:
      id      1 byte, 0xFF -> uint32 follows      (always 0 here: a
                                                   descriptor-definition
                                                   chunk)
      length  1 byte, 0xFF -> uint32 follows
      value   <length> bytes:
                uint16  the descriptor id being defined
                text    NUL-terminated, newline-separated <TAG>value lines

The text is stored verbatim, because that is what the Race's map holds and
what generate_sbem_tables.py parses.
"""

import json
import struct
import sys

MAGIC = b"SBEM0103"


def parse(blob):
    if not blob.startswith(MAGIC):
        raise SystemExit("not an SBEM0103 container (starts %r)" % blob[:8])

    out = {}
    pos = len(MAGIC)
    while pos < len(blob):
        chunk_id = blob[pos]
        pos += 1
        if chunk_id == 0xFF:
            chunk_id = struct.unpack_from("<I", blob, pos)[0]
            pos += 4
        if pos >= len(blob):
            break
        length = blob[pos]
        pos += 1
        if length == 0xFF:
            length = struct.unpack_from("<I", blob, pos)[0]
            pos += 4

        value = blob[pos:pos + length]
        pos += length
        if len(value) < length:
            raise SystemExit("truncated chunk at %d" % pos)

        # Only the descriptor-definition chunks are of interest. Anything
        # else is reported rather than skipped silently, because an
        # unexpected chunk id means this file is not what it looks like.
        if chunk_id != 0:
            print("note: ignoring chunk id %d (%d bytes)" % (chunk_id, length),
                  file=sys.stderr)
            continue
        if length < 3:
            continue

        descriptor_id = struct.unpack_from("<H", value, 0)[0]
        text = value[2:].split(b"\0", 1)[0].decode("utf-8")
        if str(descriptor_id) in out:
            raise SystemExit("descriptor %d defined twice" % descriptor_id)
        out[str(descriptor_id)] = text

    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    blob = open(sys.argv[1], "rb").read()
    table = parse(blob)
    # Sorted numerically so a diff between two watches is readable.
    ordered = {k: table[k] for k in sorted(table, key=int)}
    with open(sys.argv[2], "w") as f:
        json.dump(ordered, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print("%d descriptors -> %s" % (len(ordered), sys.argv[2]))


if __name__ == "__main__":
    main()
