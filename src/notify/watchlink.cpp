#include "watchlink.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
// Beside the database rather than in /tmp: the application's data directory
// is the one path both processes already agree on, and Sailjail persists it
// for the application. /run/user would be tidier and the sandbox does not
// give the application a shared view of it.
const char *const kLockFileName = "watch-link.lock";
} // namespace

WatchLink::WatchLink(const QString &dataDirectory, Role role)
    : m_path(QDir(dataDirectory).filePath(QString::fromLatin1(kLockFileName)))
    , m_role(role)
{
}

WatchLink::~WatchLink()
{
    release();
}

bool WatchLink::tryClaim(QString *error)
{
    if (m_fd >= 0)
        return true;

    QDir().mkpath(QFileInfo(m_path).absolutePath());

    const int fd = ::open(QFile::encodeName(m_path).constData(),
                           O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(m_path, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    // LOCK_NB: never block. The caller decides what to do about a busy
    // lock, and the two callers decide differently on purpose.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno;
        ::close(fd);
        if (error) {
            *error = saved == EWOULDBLOCK
                    ? QStringLiteral("the other process is using the watch")
                    : QString::fromLocal8Bit(strerror(saved));
        }
        return false;
    }

    m_fd = fd;
    return true;
}

void WatchLink::release()
{
    if (m_fd < 0)
        return;
    // Closing the descriptor releases the lock; so does the process dying,
    // which is the whole reason for using one.
    ::close(m_fd);
    m_fd = -1;
}
