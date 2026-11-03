# Rewrites a SoundFont so fixed-pitch zones (scaleTuning 0, no overridingRootKey)
# keep their pitch under Wine, which ignores scaleTuning. Each one is split into
# a zone per key, rooted on that key. Usage: fix-soundfont.py <input.sf2> <output>
import struct
import sys

GEN_KEYRANGE, GEN_SAMPLE, GEN_SCALE, GEN_ROOT = 43, 53, 56, 58

source, destination = sys.argv[1], sys.argv[2]
data = open(source, 'rb').read()
if data[:4] != b'RIFF' or data[8:12] != b'sfbk':
    sys.exit('%s is not a SoundFont' % source)

chunks = []
pos = 12
while pos < len(data):
    chunk_id, size = struct.unpack('<4sI', data[pos:pos + 8])
    chunks.append((chunk_id, data[pos + 8:pos + 8 + size]))
    pos += 8 + size + (size & 1)

pdta_index = next(i for i, c in enumerate(chunks) if c[0] == b'LIST' and c[1][:4] == b'pdta')
pdta = chunks[pdta_index][1]
order = []
sub = {}
pos = 4
while pos < len(pdta):
    chunk_id, size = struct.unpack('<4sI', pdta[pos:pos + 8])
    order.append(chunk_id.decode())
    sub[order[-1]] = pdta[pos + 8:pos + 8 + size]
    pos += 8 + size


def records(name, size):
    return [sub[name][i:i + size] for i in range(0, len(sub[name]), size)]


inst = [(r[:20], struct.unpack('<H', r[20:22])[0]) for r in records('inst', 22)]
ibag = [struct.unpack('<HH', r) for r in records('ibag', 4)]
imod = records('imod', 10)
igen = [struct.unpack('<HH', r) for r in records('igen', 4)]

new_inst, new_bag, new_mod, new_gen = [], [], [], []
split = 0

for i in range(len(inst) - 1):
    name, first_bag = inst[i]
    new_inst.append((name, len(new_bag)))
    global_gens = {}
    for bag in range(first_bag, inst[i + 1][1]):
        gens = igen[ibag[bag][0]:ibag[bag + 1][0]]
        mods = imod[ibag[bag][1]:ibag[bag + 1][1]]
        opers = dict(gens)
        is_global = GEN_SAMPLE not in opers
        if is_global and bag == first_bag:
            global_gens = opers
        scale = opers.get(GEN_SCALE, global_gens.get(GEN_SCALE, 100))
        root = opers.get(GEN_ROOT, global_gens.get(GEN_ROOT, 0xFFFF))
        if is_global or scale != 0 or root != 0xFFFF:
            new_bag.append((len(new_gen), len(new_mod)))
            new_gen.extend(gens)
            new_mod.extend(mods)
            continue
        key_range = opers.get(GEN_KEYRANGE, 0x7F00)
        split += 1
        # keyRange must stay first and sampleID last.
        for key in range(key_range & 0xFF, (key_range >> 8) + 1):
            new_bag.append((len(new_gen), len(new_mod)))
            new_gen.append((GEN_KEYRANGE, key | (key << 8)))
            new_gen.extend((o, a) for o, a in gens if o not in (GEN_KEYRANGE, GEN_SAMPLE, GEN_ROOT))
            new_gen.append((GEN_ROOT, key))
            new_gen.append((GEN_SAMPLE, opers[GEN_SAMPLE]))
            new_mod.extend(mods)

new_inst.append((inst[-1][0], len(new_bag)))
new_bag.append((len(new_gen), len(new_mod)))
new_gen.append(igen[-1])
new_mod.append(imod[-1])
if len(new_bag) > 0xFFFF or len(new_gen) > 0xFFFF:
    sys.exit('The fixed SoundFont would exceed the format limits')

sub['inst'] = b''.join(n + struct.pack('<H', b) for n, b in new_inst)
sub['ibag'] = b''.join(struct.pack('<HH', *r) for r in new_bag)
sub['imod'] = b''.join(new_mod)
sub['igen'] = b''.join(struct.pack('<HH', *r) for r in new_gen)

chunks[pdta_index] = (b'LIST', b'pdta' + b''.join(
    name.encode() + struct.pack('<I', len(sub[name])) + sub[name] for name in order))
riff = b'sfbk' + b''.join(
    c + struct.pack('<I', len(d)) + d + (b'\0' if len(d) & 1 else b'') for c, d in chunks)
open(destination, 'wb').write(b'RIFF' + struct.pack('<I', len(riff)) + riff)
print('Split %d fixed-pitch zones' % split)
