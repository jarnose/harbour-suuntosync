#!/usr/bin/env python3
"""Retrofit's own words: the @GET/@PUT path and the @Query/@Header names.

androguard reads classes and code but hands annotations back as offsets, so
this walks the dex's annotation structures directly. The layout is the
documented one: annotations_directory -> annotation_set -> annotation_item
-> encoded_annotation, with uleb128 indices into the string table.
"""
import sys, struct
from loguru import logger; logger.remove()
from androguard.core.dex import DEX

data = open(sys.argv[1], 'rb').read()
d = DEX(data)
needle = sys.argv[2]

def uleb(off):
    result = 0; shift = 0
    while True:
        b = data[off]; off += 1
        result |= (b & 0x7f) << shift
        if not (b & 0x80):
            return result, off
        shift += 7

def string_at(idx):
    # string_ids are 4-byte offsets to (uleb128 length, MUTF-8 bytes)
    string_ids_off = struct.unpack_from('<I', data, 60)[0]
    off = struct.unpack_from('<I', data, string_ids_off + 4 * idx)[0]
    _, off = uleb(off)
    end = data.index(b'\x00', off)
    return data[off:end].decode('utf-8', 'replace')

def type_name(idx):
    type_ids_off = struct.unpack_from('<I', data, 68)[0]
    return string_at(struct.unpack_from('<I', data, type_ids_off + 4 * idx)[0])

def encoded_value(off):
    arg_and_type = data[off]; off += 1
    vtype = arg_and_type & 0x1f
    varg = arg_and_type >> 5
    if vtype == 0x17:  # string
        idx = int.from_bytes(data[off:off + varg + 1], 'little')
        return string_at(idx), off + varg + 1
    if vtype == 0x1c:  # array
        size, off = uleb(off)
        items = []
        for _ in range(size):
            value, off = encoded_value(off)
            items.append(value)
        return items, off
    if vtype in (0x00, 0x02, 0x03, 0x04, 0x06):  # small ints
        return int.from_bytes(data[off:off + varg + 1], 'little'), off + varg + 1
    if vtype == 0x1e:  # null
        return None, off
    if vtype == 0x1f:  # boolean, in the arg
        return bool(varg), off
    return '<type 0x%02x>' % vtype, off + varg + 1

def annotation(off):
    _visibility = data[off]; off += 1
    type_idx, off = uleb(off)
    size, off = uleb(off)
    elements = {}
    for _ in range(size):
        name_idx, off = uleb(off)
        value, off = encoded_value(off)
        elements[string_at(name_idx)] = value
    return type_name(type_idx), elements

def annotation_set(off):
    if not off:
        return []
    size = struct.unpack_from('<I', data, off)[0]
    out = []
    for i in range(size):
        item_off = struct.unpack_from('<I', data, off + 4 + 4 * i)[0]
        out.append(annotation(item_off))
    return out

def annotation_set_refs(off):
    if not off:
        return []
    size = struct.unpack_from('<I', data, off)[0]
    return [annotation_set(struct.unpack_from('<I', data, off + 4 + 4 * i)[0])
            for i in range(size)]

def short(name):
    return name.rsplit('/', 1)[-1].rstrip(';')

for cls in d.get_classes():
    if needle not in cls.get_name():
        continue
    adi = cls.annotations_directory_item
    if adi is None:
        continue
    print('===', cls.get_name())
    methods = {m.get_method_idx(): m for m in cls.get_methods()}
    params = {pa.get_method_idx(): pa for pa in adi.get_parameter_annotations()}
    for ma in adi.get_method_annotations():
        idx = ma.get_method_idx()
        name = methods[idx].get_name() if idx in methods else '?'
        anns = annotation_set(ma.get_annotations_off())
        line = ' '.join('%s%s' % (short(t), e if e else '') for t, e in anns)
        print('  %-24s %s' % (name, line))
        if idx in params:
            for i, pset in enumerate(annotation_set_refs(params[idx].get_annotations_off())):
                if pset:
                    print('        arg%-2d %s' % (i, ' '.join(
                        '%s%s' % (short(t), e if e else '') for t, e in pset)))
