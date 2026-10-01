#!/usr/bin/env python3
"""Pulls Whiteboard frames out of an Android btsnoop HCI capture.

    python3 tools/btsnoop_mds.py capture.log                 # every frame
    python3 tools/btsnoop_mds.py capture.log --type 0e       # PUTs only
    python3 tools/btsnoop_mds.py capture.log --reqid 0x026e  # one exchange
    python3 tools/btsnoop_mds.py capture.log --path Ancs     # GETs matching
    python3 tools/btsnoop_mds.py capture.log --save out/     # bodies to files

Every protocol finding in docs/ came out of a capture like this, and the
code that read one has been rewritten from scratch three or four times.
This is that step, kept.

The chain is btsnoop record -> HCI ACL -> L2CAP -> ATT -> the SLIP framing
in src/ble/mdswirecodec.h:

  - btsnoop: a 16-byte file header, then 24-byte record headers. Bit 0 of
    the per-record flags is the direction, which is how watch->phone is
    told from phone->watch.
  - The payload is H4: a 1-byte type, 0x02 for ACL. An ACL header carries
    a 12-bit handle and a 2-bit packet-boundary flag; only first fragments
    (PB 0 or 2) start an L2CAP packet.
  - L2CAP channel 4 is ATT. Opcodes 0x52 (Write Command) and 0x12 (Write
    Request) are the phone writing, 0x1b (Handle Value Notification) is the
    watch answering.

**Streams are keyed by ATT handle, and that matters.** A Race writes on
0x0012 and notifies on 0x0015; a Suunto 9 Baro uses 0x000e and 0x0010. A
capture containing both watches reassembles into nonsense unless each
handle is kept apart, which is why the grouping below is not an
optimisation.

CRCs are not checked. A capture is not a radio: if a frame is in the file
it arrived, and a frame whose length field is wrong is more useful shown
than silently dropped while you are trying to work out why.
"""

import argparse
import os
import struct
import sys

H4_ACL = 0x02
L2CAP_ATT_CID = 4
ATT_WRITE_COMMAND = 0x52
ATT_WRITE_REQUEST = 0x12
ATT_HANDLE_VALUE_NOTIFICATION = 0x1B

SLIP_END = 0x7E
SLIP_ESCAPE = 0x7D
WHITEBOARD_SYNC = 0xA5


def records(path):
    """Yields (flags, payload) for every record in a btsnoop file."""
    with open(path, "rb") as handle:
        header = handle.read(16)
        if header[:8] != b"btsnoop\x00":
            raise SystemExit("%s is not a btsnoop capture" % path)
        while True:
            record = handle.read(24)
            if len(record) < 24:
                return
            _original, included, flags, _drops, _ts = struct.unpack(">IIIIq", record)
            yield flags, handle.read(included)


def att_writes(path):
    """Yields (direction, att_handle, value) for every ATT write or notify.

    direction is "w" for phone->watch and "n" for watch->phone.
    """
    for flags, data in records(path):
        if len(data) < 9 or data[0] != H4_ACL:
            continue
        handle_and_flags = struct.unpack("<H", data[1:3])[0]
        boundary = (handle_and_flags >> 12) & 0x3
        if boundary not in (0, 2):
            continue  # a continuation of an L2CAP packet already seen
        l2cap_length, cid = struct.unpack("<HH", data[5:9])
        if cid != L2CAP_ATT_CID:
            continue
        att = data[9:9 + l2cap_length]
        if len(att) < 3:
            continue
        opcode = att[0]
        att_handle = struct.unpack("<H", att[1:3])[0]
        if opcode in (ATT_WRITE_COMMAND, ATT_WRITE_REQUEST):
            yield "w", att_handle, att[3:]
        elif opcode == ATT_HANDLE_VALUE_NOTIFICATION:
            yield "n", att_handle, att[3:]


def slip_frames(stream):
    """Splits a byte stream on 0x7E and unescapes each frame."""
    frames = []
    current = None
    escaped = False
    for byte in stream:
        if byte == SLIP_END:
            if current is None:
                current = bytearray()
            else:
                if current:
                    frames.append(bytes(current))
                current = None
            escaped = False
        elif current is None:
            continue  # bytes before the first delimiter
        elif escaped:
            current.append(0x7E if byte == 0x5E else 0x7D if byte == 0x5D else byte)
            escaped = False
        elif byte == SLIP_ESCAPE:
            escaped = True
        else:
            current.append(byte)
    return frames


def whiteboard_frames(path):
    """Yields (direction, att_handle, type, request_id, body)."""
    streams = {}
    for direction, att_handle, value in att_writes(path):
        streams.setdefault((direction, att_handle), bytearray()).extend(value)

    for (direction, att_handle), stream in sorted(streams.items()):
        for frame in slip_frames(stream):
            if len(frame) < 10 or frame[0] != WHITEBOARD_SYNC:
                continue
            frame_type = frame[1]
            length = struct.unpack("<H", frame[2:4])[0]
            request_id = struct.unpack("<H", frame[4:6])[0]
            yield direction, att_handle, frame_type, request_id, frame[6:6 + length]


def describe(frame_type):
    return {
        0x01: "bulk",
        0x02: "ack",
        0x05: "response",
        0x07: "put-resp",
        0x08: "stream-ack",
        0x0A: "GET",
        0x0D: "handle-fetch",
        0x0E: "PUT",
        0x10: "stream-start",
        0x11: "stream-stop",
        0x12: "handshake",
        0x13: "handshake-resp",
    }.get(frame_type, "")


def path_of(body):
    """The ASCII resource path in a GET body, or None."""
    if len(body) < 5:
        return None
    try:
        return body[4:].decode("ascii").rstrip("\x00")
    except UnicodeDecodeError:
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("capture")
    parser.add_argument("--type", help="only this frame type, in hex (e.g. 0e)")
    parser.add_argument("--reqid", help="only this request id (e.g. 0x026e)")
    parser.add_argument("--path", help="only GETs whose path contains this text")
    parser.add_argument("--save", metavar="DIR", help="write each body to a .bin there")
    parser.add_argument("--max-hex", type=int, default=0,
                        help="truncate printed hex to this many bytes (0 = all)")
    args = parser.parse_args()

    wanted_type = int(args.type, 16) if args.type else None
    wanted_reqid = int(args.reqid, 0) if args.reqid else None
    if args.save:
        os.makedirs(args.save, exist_ok=True)

    shown = 0
    for direction, att_handle, frame_type, request_id, body in whiteboard_frames(args.capture):
        if wanted_type is not None and frame_type != wanted_type:
            continue
        if wanted_reqid is not None and request_id != wanted_reqid:
            continue
        resource = path_of(body) if frame_type == 0x0A else None
        if args.path and (resource is None or args.path not in resource):
            continue

        label = describe(frame_type)
        print("%s att=0x%04x type=0x%02x%s reqid=0x%04x len=%d%s"
              % ("-->" if direction == "w" else "<--", att_handle, frame_type,
                 (" " + label) if label else "", request_id, len(body),
                 ("  " + resource) if resource else ""))
        payload = body[:args.max_hex] if args.max_hex else body
        print("    " + payload.hex() + ("..." if len(payload) < len(body) else ""))

        if args.save:
            name = "%s_%02x_%04x_%d.bin" % (direction, frame_type, request_id, shown)
            with open(os.path.join(args.save, name), "wb") as out:
                out.write(body)
        shown += 1

    if shown == 0:
        print("no frames matched", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
