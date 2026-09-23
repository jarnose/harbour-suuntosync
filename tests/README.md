# Tests

Every decoder in this project is Qt-free on purpose, so it can be compiled
and run with plain `g++` on a development machine rather than only on the
phone. That matters because there is no Qt/Sailfish toolchain in the
environment most of this was written in, and because a protocol decoder is
exactly the kind of code where "it compiles" proves nothing.

```
g++ -std=c++17 ../src/cloud/iso8601.cpp test_iso8601.cpp -o /tmp/t && /tmp/t
```

Each test file's header comment gives its own command line.

## The fixtures are not in this repository

`tests/fixtures/` is gitignored. The files in it are real captures taken
off a real watch and a real account, and they are personal data:

- **GPS tracks** whose first point is the owner's home, to within a few
  metres. Two different workouts on different days start from the same
  spot, which is exactly what makes it identifying.
- **Twelve nights of sleep**, with heart rate, HRV, SpO2 and sleep quality.
- **The watch serial**, which appears in several of them.

They are kept on the machine they were captured on. The repository carries
the decoders, the documentation and the reasoning; it does not carry
somebody's movements and heart rate.

## What that means if you cloned this

The golden-vector tests will not run - they read files that are not here.
Everything else does: the code, the protocol documentation in `docs/`, and
the commit history, which is where the actual reverse-engineering argument
lives.

To regenerate equivalents from your own watch, the paths are:

| fixture | where it comes from |
|---|---|
| `logbook_data_heatshrink*.bin` | the bulk stream from `/Logbook/byId/<id>/Data` |
| `logbook_summary_cycling.bin` | the paged reads from `/Logbook/byId/<id>/Summary` |
| `sleep_timeline.sbem` | `/Daily/Sleep/Timeline/Data`, then `mdsSlp.sbm` off `/Dev/FileSystem/Stream` |
| `cloud_sml_*.json` | the `sml.zip` part of a captured `POST /v1/workout` |
| `cloud_247_v1_*.json` | a captured `POST` to `247.sports-tracker.com/v1/<kind>` |
| `workout_binary_*.bin` | the `workoutBinary` part of a phone-recorded upload |

`docs/logbook-data-format.md` and `docs/watch-push-resources.md` describe
each of those exchanges in enough detail to repeat them. The app's own
probe actions (on the pairing page) write the raw payloads to the cache
directory, which is how most of these were obtained.

The expected values in the tests are deliberately *not* derived from this
project's own output. Where possible they come from a second, independent
source - the Suunto cloud's own figures for the same workout or the same
night, Python's `datetime` for epoch conversions, or the published example
in a format's specification. That is what makes them worth running.
