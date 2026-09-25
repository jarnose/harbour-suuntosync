#!/usr/bin/env python3
"""Generates src/ble/sbemdescriptors.{h,cpp} from docs/sbem-descriptor-map.json.

The JSON is the watch's own descriptor table, captured over BLE and parsed
with the chunk rules decompiled from libmds.so - see
docs/logbook-data-format.md. Turning it into a C++ table means the decoder
works the way the official app does (look the field up, don't hard-code a
byte offset), and adding a field costs nothing.

The generated files are committed, so a normal build never runs this. Re-run
it only if the descriptor map itself is replaced (a different watch model or
firmware would produce a different one).

    python3 tools/generate_sbem_tables.py
"""

import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAP = os.path.join(ROOT, "docs", "sbem-descriptor-map.json")
OUT_H = os.path.join(ROOT, "src", "ble", "sbemdescriptors.h")
OUT_C = os.path.join(ROOT, "src", "ble", "sbemdescriptors.cpp")

# Byte size per format base type. Matches BSML::DecoderBase::getFormatSize()
# as decompiled from libmds.so; utf8 is variable (NUL-terminated) and d0 is
# a zero-byte "unchanged" marker.
SIZES = {
    "uint8": 1, "int8": 1, "bool": 1, "enum": 1,
    "uint16": 2, "int16": 2, "dint16": 2,
    "uint32": 4, "int32": 4, "float32": 4,
    "float64": 8, "local64": 8,
    "dint8": 1, "duint8": 1,
    "d0": 0,
    "utf8": -1,
}

# Every <MOD> decode expression in the captured table reduces to a linear
# value = raw * scale + offset. Listed literally rather than parsed so an
# unrecognised one fails loudly instead of being silently mis-scaled.
#
# Latitude/longitude deliberately deviate: the schema converts to radians
# (PI*x/(10^7*180)) because that is what the app works in, but degrees are
# what everything else in this project uses, so they get 1e-7 instead.
MODS = {
    "x/1000": (1.0 / 1000, 0.0),
    "x/10": (1.0 / 10, 0.0),
    "x/5": (1.0 / 5, 0.0),
    "x/50": (1.0 / 50, 0.0),
    "x/60": (1.0 / 60, 0.0),
    "x/100": (1.0 / 100, 0.0),
    "x/(1000*128)": (1.0 / 128000, 0.0),
    "x/5-1000": (1.0 / 5, -1000.0),
    "x+85000": (1.0, 85000.0),
    "x*3.6": (3.6, 0.0),
    "PI*x/(10^7*180)": (1e-7, 0.0),  # degrees, see above
    # The 9 Baro has a compass; the Race does not. Raw is 1/100 degree and
    # the schema converts to radians, but degrees are what this project
    # uses everywhere else - same deliberate deviation as latitude above.
    "PI*x/100/180": (1.0 / 100, 0.0),
}


def short_path(text):
    m = re.search(r"<PTH>([^\n]*)", text)
    if not m:
        return ""
    return (m.group(1)
            .replace("Samples.TimelineSample.Attributes.suunto/sml.", "")
            .replace("Samples+TimelineSample.", "")
            .replace("Samples.TimelineSample.", ""))


def parse(descriptors):
    out = {}
    for did, text in descriptors.items():
        entry = {
            "id": did, "name": short_path(text), "children": [],
            "base": "", "size": 0, "scale": 1.0, "offset": 0.0,
            "nil": None, "delta_of": 0, "precision": -1,
        }

        group = re.match(r"<GRP>([\d,]+)", text)
        if group:
            entry["children"] = [int(x) for x in group.group(1).split(",")]
            out[did] = entry
            continue

        delta = re.search(r"<DELTAREF>(\d+)", text)
        if delta:
            entry["delta_of"] = int(delta.group(1))

        frm = re.search(r"<FRM>([^\n]*)", text)
        if frm:
            parts = frm.group(1).split(",")
            base = parts[0]
            if base.startswith("enum:"):
                base = "enum"
            if base not in SIZES:
                raise SystemExit("unknown format base type: %r (descriptor %d)" % (base, did))
            entry["base"] = base
            entry["size"] = SIZES[base]
            for part in parts[1:]:
                if part.startswith("nillable="):
                    entry["nil"] = float(part.split("=", 1)[1])
                elif part.startswith("precision="):
                    # Decimal places for *output*, not storage - the cloud's
                    # own JSON rounds to this (Sample.HR is precision=2, and
                    # a captured upload has "HR": 1.33 where the raw value is
                    # 80/60 = 1.3333...). Fields without it, e.g.
                    # Sample.Altitude, are written at full precision, which
                    # is why the capture has 205.4000000000001.
                    entry["precision"] = int(part.split("=", 1)[1])

        mod = re.search(r"<MOD>([^\n]*)", text)
        if mod:
            expr = mod.group(1).split(",")[0]
            if expr not in MODS:
                raise SystemExit("unknown <MOD> expression: %r (descriptor %d)" % (expr, did))
            entry["scale"], entry["offset"] = MODS[expr]

        out[did] = entry
    return out


def cpp_enum(base):
    return {
        "uint8": "Format::UInt8", "int8": "Format::Int8", "bool": "Format::Bool",
        "enum": "Format::Enum", "uint16": "Format::UInt16", "int16": "Format::Int16",
        "dint16": "Format::DeltaInt16", "uint32": "Format::UInt32", "int32": "Format::Int32",
        "float32": "Format::Float32", "float64": "Format::Float64", "local64": "Format::Local64",
        "dint8": "Format::DeltaInt8", "duint8": "Format::DeltaUInt8", "d0": "Format::Unchanged",
        "utf8": "Format::Utf8", "": "Format::None",
    }[base]


def main():
    with open(MAP) as f:
        raw = json.load(f)
    descriptors = parse({int(k): v for k, v in raw.items()})
    ids = sorted(descriptors)

    banner = ("// GENERATED by tools/generate_sbem_tables.py from\n"
              "// docs/sbem-descriptor-map.json - do not edit by hand.\n")

    with open(OUT_H, "w") as f:
        f.write(banner + """
#pragma once

#include <cstddef>
#include <cstdint>

// The watch's own field table, as captured from its /Descriptors resource.
// A chunk id in a /Data or /Summary payload is a descriptor id, and a group
// descriptor's payload is its children's values concatenated in order - so
// this table is all a decoder needs. See docs/sbem-chunk-map.md for the
// human-readable version and docs/logbook-data-format.md for how it was
// obtained.
namespace SbemDescriptors {

enum class Format {
    None, Bool, Enum, UInt8, Int8, UInt16, Int16, UInt32, Int32,
    Float32, Float64, Local64, Utf8,
    // Differential formats: the value is a delta against deltaOf's running
    // value rather than a reading of its own.
    DeltaInt8, DeltaUInt8, DeltaInt16,
    // Zero-byte "same as last time" marker.
    Unchanged,
};

struct Descriptor {
    uint16_t id;
    const char *name;       // dotted path, common prefixes stripped; "" for a group
    Format format;
    int8_t size;            // bytes; -1 for utf8 (NUL-terminated), 0 for Unchanged
    double scale;           // value = raw * scale + offset (the schema's <MOD>)
    double offset;
    bool hasNil;            // raw == nil means "no reading"
    double nil;
    uint16_t deltaOf;       // descriptor this is a delta against, 0 if none
    int8_t precision;       // decimal places when written out; -1 = full
    const uint16_t *children;
    uint16_t childCount;    // >0 means this is a group
};

// Returns nullptr for an id the captured table doesn't describe.
const Descriptor *find(uint16_t id);

// The whole built-in table, for code that has to walk it rather than look
// one id up - SbemLayout::resolve() does, because it has to find which
// group carries a field rather than being told.
const Descriptor *all(size_t *count);

} // namespace SbemDescriptors
""")

    with open(OUT_C, "w") as f:
        f.write(banner + """
#include "sbemdescriptors.h"

namespace SbemDescriptors {
namespace {

""")
        for did in ids:
            d = descriptors[did]
            if d["children"]:
                f.write("const uint16_t kChildren%d[] = {%s};\n"
                        % (did, ", ".join(str(c) for c in d["children"])))
        f.write("\nconst Descriptor kDescriptors[] = {\n")
        for did in ids:
            d = descriptors[did]
            children = "kChildren%d" % did if d["children"] else "nullptr"
            f.write('    {%d, "%s", %s, %d, %r, %r, %s, %r, %d, %d, %s, %d},\n' % (
                did, d["name"], cpp_enum(d["base"]), d["size"],
                d["scale"], d["offset"],
                "true" if d["nil"] is not None else "false",
                d["nil"] if d["nil"] is not None else 0.0,
                d["delta_of"], d["precision"], children, len(d["children"])))
        f.write("};\n\n} // namespace\n\n")
        f.write("""const Descriptor *find(uint16_t id)
{
    // The table is sorted by id but sparse, so binary search rather than
    // index directly.
    size_t low = 0;
    size_t high = sizeof(kDescriptors) / sizeof(kDescriptors[0]);
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (kDescriptors[mid].id == id)
            return &kDescriptors[mid];
        if (kDescriptors[mid].id < id)
            low = mid + 1;
        else
            high = mid;
    }
    return nullptr;
}

const Descriptor *all(size_t *count)
{
    if (count)
        *count = sizeof(kDescriptors) / sizeof(kDescriptors[0]);
    return kDescriptors;
}

} // namespace SbemDescriptors
""")
    print("wrote %s and %s (%d descriptors)" % (OUT_H, OUT_C, len(ids)))


if __name__ == "__main__":
    main()
