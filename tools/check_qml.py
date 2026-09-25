#!/usr/bin/env python3
"""Catches the QML mistakes that a C++ build cannot, and that show up on
the phone as a blank screen.

Written after one of them did exactly that: a second Component.onCompleted
was added to MainPage without noticing the file already had one. QML
rejects a duplicate signal handler on the same object, the page fails to
load, and the app starts to nothing. The compiler is no help - QML is data
files - so this runs over them instead.

Tracks brace depth to tell objects apart, because handler names repeat
legitimately across sibling elements (five MenuItems each with onClicked
is fine; one element with two onClicked is not).
"""
import re
import sys
import glob
import collections

QT56_BANNED = {
    r'\btopPadding\b': 'padding properties need QtQuick 2.6',
    r'\bbottomPadding\b': 'padding properties need QtQuick 2.6',
    r'\bQt\.callLater\b': 'Qt.callLater is Qt 5.8+',
    r'\bCover\.Activating\b': 'Cover.Activating is not a CoverBackground property',
}


def duplicate_handlers(path):
    """Yields (line, name) for handlers declared twice on one object."""
    problems = []
    depth = 0
    # handlers[depth] -> {name: first line}, reset when a scope closes
    seen = collections.defaultdict(dict)
    for number, line in enumerate(open(path), 1):
        code = re.sub(r'//.*$', '', line)
        handler = re.match(r'\s*(Component\.onCompleted|on[A-Z]\w*)\s*:', code)
        if handler:
            name = handler.group(1)
            if name in seen[depth]:
                problems.append((number, name, seen[depth][name]))
            else:
                seen[depth][name] = number
        for char in code:
            if char == '{':
                depth += 1
            elif char == '}':
                seen.pop(depth, None)   # leaving the scope forgets its handlers
                depth -= 1
    return problems


def main():
    files = sorted(glob.glob('qml/**/*.qml', recursive=True))
    problems = 0
    for path in files:
        source = open(path).read()

        for line, name, first in duplicate_handlers(path):
            print('%s:%d: %s already declared on this object at line %d'
                  % (path, line, name, first))
            problems += 1

        for pattern, why in QT56_BANNED.items():
            for match in re.finditer(pattern, source):
                line = source[:match.start()].count('\n') + 1
                print('%s:%d: %s' % (path, line, why))
                problems += 1

        stripped = re.sub(r'//[^\n]*', '', re.sub(r'"(\\.|[^"\\])*"', '""', source))
        for opener, closer, name in (('{', '}', 'braces'), ('(', ')', 'parens')):
            if stripped.count(opener) != stripped.count(closer):
                print('%s: unbalanced %s (%d vs %d)'
                      % (path, name, stripped.count(opener), stripped.count(closer)))
                problems += 1

    print('checked %d QML files, %d problem(s)' % (len(files), problems))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
