#pragma once

#include <QString>

// The watch the user has chosen within this app (BlueZ-level "Connect()"
// target). Separate from BlueZ's own pairing/bonding state - see
// BluezAdapter's class comment for why this app relies on Settings >
// Bluetooth for the actual pairing step.
struct PairedWatch
{
    QString address;    // e.g. "0C:8C:DC:C2:13:59"
    QString objectPath; // e.g. "/org/bluez/hci0/dev_0C_8C_DC_C2_13_59"
    QString name;       // e.g. "Suunto Race 2352D0000247"
    QString model;      // "Race", "9 Baro", ... - free text for now (Phase 8)

    bool isValid() const { return !address.isEmpty(); }
};
