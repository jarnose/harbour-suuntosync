#!/usr/bin/env python3
"""Read the official app's dex files without a decompiler.

This is what produced docs/wifi-and-offline-maps.md, and it is here because
the technique kept being rediscovered: there is no jadx in this
environment, but `androguard` is installed, and for answering "what
resource does this method talk to" and "what are this payload's field
names" a decompiler is not needed.

Three things are worth knowing about reading Kotlin/Moshi code this way:

  * a method's string constants, in order, usually tell the whole story -
    a resource path, then the shape of what is written to it;
  * a generated Moshi adapter's constructor passes the **wire** field names
    to JsonReader.Options.of(...), which is the authoritative spelling
    (/Device/Connectivity/Wifi/Connect takes `Ssid`, not `ssid`);
  * an enum's <clinit> carries each constant's numeric value, except when
    the compiler reuses one register for the ordinal and the value - then
    only one number shows, and value-equals-ordinal is a reading rather
    than a fact.

Retrofit paths and @Query names live in annotations, which this does not
read; this project's docs/app-api-surface.txt already has the paths.

Usage, where <dex> is one classes*.dex out of an unzipped APK:

  classes <dex> <substring>        class names containing the substring
  dump    <dex> <substr>[,<substr>]  each matching class: methods, their
                                   string constants and the calls they make
  methods <dex> <class> <m>[,<m>]  only these methods, in full
  users   <dex> <string>           which method mentions this string
"""

import re
import sys

try:
    from loguru import logger
    logger.remove()  # androguard logs its whole parse at DEBUG otherwise
except ImportError:
    pass

from androguard.core.dex import DEX


def short(name):
    # The package prefixes are the same on every line and crowd out the
    # part that differs.
    name = re.sub(r'L(?:com/suunto|com/stt)/[A-Za-z0-9/$]*/', '', name)
    return re.sub(r'L[a-z0-9]{1,4}/[A-Za-z0-9$]*;', 'X;', name)


def load(path):
    return DEX(open(path, 'rb').read())


def instructions(method, calls=True):
    out = []
    try:
        for ins in method.get_instructions():
            op, arg = ins.get_name(), ins.get_output()
            if op.startswith('const-string'):
                out.append('"%s"' % arg.split(',', 1)[1].strip().strip('"'))
            elif op.startswith('const/') or op.startswith('const-wide'):
                out.append('#' + arg.split(',', 1)[1].strip())
            elif calls and (op.startswith('invoke') or op.startswith('sget')):
                out.append(short(arg.split(',')[-1].strip()))
    except Exception as e:  # a malformed method should not stop the dump
        out.append('<unreadable: %s>' % e)
    return out


def cmd_classes(dex, needle):
    for cls in load(dex).get_classes():
        if needle in cls.get_name():
            print(cls.get_name())


def cmd_dump(dex, needles):
    wanted = needles.split(',')
    for cls in load(dex).get_classes():
        name = cls.get_name()
        if not any(n in name for n in wanted):
            continue
        print('=== %s' % name)
        for m in cls.get_methods():
            if m.get_name() in ('equals', 'hashCode', 'values', 'valueOf'):
                continue
            body = instructions(m)
            print('  %s%s' % (m.get_name(), m.get_descriptor()))
            if body:
                print('      %s' % ' | '.join(body[:80]))


def cmd_methods(dex, cls_needle, meth_needles):
    wanted = meth_needles.split(',')
    for cls in load(dex).get_classes():
        if cls_needle not in cls.get_name():
            continue
        for m in cls.get_methods():
            if m.get_name() not in wanted:
                continue
            print('--- %s%s' % (m.get_name(), m.get_descriptor()))
            print('    %s' % ' | '.join(instructions(m)))


def cmd_users(dex, needle):
    for cls in load(dex).get_classes():
        for m in cls.get_methods():
            hits = [s for s in instructions(m, calls=False)
                    if s.startswith('"') and needle in s]
            if hits:
                print('%s -> %s' % (cls.get_name(), m.get_name()))
                print('    %s' % ' | '.join(sorted(set(hits))))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mode = sys.argv[1]
    if mode == 'classes':
        cmd_classes(sys.argv[2], sys.argv[3])
    elif mode == 'dump':
        cmd_dump(sys.argv[2], sys.argv[3])
    elif mode == 'methods':
        cmd_methods(sys.argv[2], sys.argv[3], sys.argv[4])
    elif mode == 'users':
        cmd_users(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
