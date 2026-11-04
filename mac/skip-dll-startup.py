# Writes a copy of a 64-bit DLL whose entry point returns TRUE straight away. apm.dll's
# startup deadlocks in Wine's XInput, and OpenParrot replaces the functions that use it.
# Usage: skip-dll-startup.py <input.dll> <output.dll>
import struct
import sys

source, destination = sys.argv[1], sys.argv[2]
RETURN_TRUE = b'\xb8\x01\x00\x00\x00\xc3'  # mov eax, 1; ret

data = bytearray(open(source, 'rb').read())
if data[:2] != b'MZ':
    sys.exit('%s is not a DLL' % source)

pe = struct.unpack('<I', data[0x3c:0x40])[0]
section_count = struct.unpack('<H', data[pe + 6:pe + 8])[0]
optional_size = struct.unpack('<H', data[pe + 20:pe + 22])[0]
entry = struct.unpack('<I', data[pe + 40:pe + 44])[0]

for index in range(section_count):
    header = pe + 24 + optional_size + 40 * index
    _, address, raw_size, raw_pointer = struct.unpack('<IIII', data[header + 8:header + 24])
    if address <= entry < address + raw_size:
        offset = entry - address + raw_pointer
        data[offset:offset + len(RETURN_TRUE)] = RETURN_TRUE
        break
else:
    sys.exit('The entry point of %s was not found' % source)

open(destination, 'wb').write(data)
