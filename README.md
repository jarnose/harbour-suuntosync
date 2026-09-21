# Suunto Sync

Native Sailfish OS (C++/QML, Silica) app to sync workout history and stats
from a Suunto smartwatch (Suunto Race primarily, Suunto 9 Baro planned) and
forward phone notifications to it. Two data paths, built in parallel:

- **Cloud**: sign in to the Suunto account, pull workout history/stats from
  `cloud-api.suunto.com`.
- **Direct BLE**: pair with the watch and sync the on-device logbook and push
  notifications, using the same Movesense "Whiteboard" BLE protocol
  (`suunto://MDS/...` resource paths) the official app uses internally.

## Status

**Phase 1 (app scaffold) only** — no real features yet. The full phased plan,
including the three protocol/platform unknowns that gate the BLE and
notification work, is at
[`~/.claude/plans/agile-hopping-harp.md`](~/.claude/plans/agile-hopping-harp.md).

Short version of what's still unknown and needs an on-device capture before
the corresponding phase can be built:
- **BLE wire framing** (how a Whiteboard request/response is packed into GATT
  write/notify bytes) — needs a Bluetooth HCI snoop log captured from the
  official Android app while syncing a real Suunto Race.
- **Cloud OAuth/API schema** — needs an HTTPS capture (mitmproxy) of the
  official app's login + workout fetch.
- **Whether a sandboxed Sailfish app can observe other apps' notifications at
  all** (needed for the phone→watch bridge) — needs an on-device D-Bus check
  from an actual `sailjail`-sandboxed install, not devel-mode.

## Building

Open in Qt Creator with the Sailfish OS SDK, or via `sfdk`/`mb2` from the
command line; dependencies resolve via `pkg-config` inside the build engine.

This project has never used Qt Bluetooth before (see
`rpm/harbour-suuntosync.spec`'s note on `pkgconfig(Qt5Bluetooth)`) — if the
build fails to find it, check the exact package name inside the build engine
with `pkgconfig --list-all | grep -i bluetooth` and fix the spec/CMakeLists.

## Security model

Suunto account OAuth tokens are stored in the **Sailfish Secrets vault**
(same pattern as `harbour-otpcove`'s `src/secrets/secretvault.cpp`), never in
plain text on disk. Everything else (workout metadata, paired watch info) is
a plain SQLite database in the app's data directory.
