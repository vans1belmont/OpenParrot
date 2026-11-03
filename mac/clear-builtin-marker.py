# Copies a Wine PE DLL without its "Wine builtin DLL" marker, so that Wine loads
# this file instead of substituting its own builtin DLL of the same name.
import sys

MARKER = b'Wine builtin DLL'
OFFSET = 0x40

source, destination = sys.argv[1], sys.argv[2]
data = bytearray(open(source, 'rb').read())
if data[OFFSET:OFFSET + len(MARKER)] != MARKER:
    sys.exit('%s has no builtin marker' % source)
data[OFFSET:OFFSET + len(MARKER)] = bytes(len(MARKER))
open(destination, 'wb').write(data)
