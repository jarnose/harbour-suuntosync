#pragma once

#include <QString>

// Writes the daemon's own log to a file, because on this device the journal
// is not somewhere a log survives.
//
// Measured rather than assumed: journald here runs with Storage=volatile and
// SplitMode=none, so a user service's output goes to the system journal, in
// RAM, and the phone produces enough noise that the window is **seconds**
// wide - "Logs begin at 19:38:02" with the clock reading 19:38:11. Adding
// defaultuser to systemd-journal makes the journal readable and there is
// still nothing in it to read.
//
// A background process nobody can watch has to leave a trail somewhere, so
// this installs a Qt message handler that appends to a file and, when that
// file grows past a limit, keeps exactly one previous generation. stderr
// still gets everything, so running the daemon by hand is unchanged.
namespace FileLogger {

// `path` is created along with its directory. Returns false and leaves the
// default handler in place if the file cannot be opened - a daemon that
// cannot log is still a daemon worth starting.
bool install(const QString &path);

// Where the daemon logs unless told otherwise: beside the application's own
// cache, which is the directory both already agree on.
QString defaultPath();

} // namespace FileLogger
