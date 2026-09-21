"""Rename a Mixamo FBX's bones to the UE4 names this engine resolves by.

Mixamo auto-rigs export a `mixamorig:`-prefixed skeleton, while the engine
looks bones up by their UE4 names -- SkinnedEnemy's gun IK, the ragdoll spec in
Phy_Bandit_PhysicsAsset.T3D and DirectionalLocomotion all hardcode `pelvis`,
`hand_r`, `thigh_l` and friends. A Mixamo rig carries an equivalent for every
one of them, so a rename is all that stands between the two.

Nothing else is touched. The rig keeps its own rest pose, bone axes, bone
lengths and up axis, because a clip authored on this skeleton is played back on
this same skeleton -- there is no retargeting here and none is wanted. That is
also why bone names turn out to live in exactly one place: an FBX stores them
on `Model` nodes, while skin clusters and animation curves bind to those models
by object id, so renaming the model carries the whole rig with it.

    python mixamo-to-ue.py <input.fbx> <output.fbx>

Binary FBX only (Mixamo exports 7700); an ASCII file is rejected rather than
silently copied.
"""
import struct
import sys

# Mixamo's bone names, less the `mixamorig:` prefix, mapped onto UE4's. The
# fingers and the mirrored limbs are generated rather than spelled out.
NAMES = {
    'Hips': 'pelvis',
    'Spine': 'spine_01', 'Spine1': 'spine_02', 'Spine2': 'spine_03',
    'Neck': 'neck_01', 'Head': 'head',
}
for side, tag in (('Left', 'l'), ('Right', 'r')):
    NAMES.update({
        side + 'Shoulder': 'clavicle_' + tag,
        side + 'Arm': 'upperarm_' + tag,
        side + 'ForeArm': 'lowerarm_' + tag,
        side + 'Hand': 'hand_' + tag,
        side + 'UpLeg': 'thigh_' + tag,
        side + 'Leg': 'calf_' + tag,
        side + 'Foot': 'foot_' + tag,
        side + 'ToeBase': 'ball_' + tag,
    })
    for finger in ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'):
        for joint in (1, 2, 3):
            NAMES['%sHand%s%d' % (side, finger, joint)] = \
                '%s_0%d_%s' % (finger.lower(), joint, tag)

# The leaf markers Mixamo appends carry no animation curves and no skin
# weights, and the engine never asks for them, so they keep their own names.
PREFIX = 'mixamorig:'


def rename(raw):
    """`b'mixamorig:LeftUpLeg\x00\x01Model'` -> `b'thigh_l\x00\x01Model'`."""
    head, sep, tail = raw.partition(b'\x00')
    if sep == b'':
        return raw
    name = head.decode('utf8', 'replace')
    if not name.startswith(PREFIX):
        return raw
    mapped = NAMES.get(name[len(PREFIX):])
    if mapped is None:
        # An unmapped bone still loses the prefix: leaving one behind would let
        # a later `mixamorig:` check pass a file that is only half converted.
        mapped = name[len(PREFIX):]
    return mapped.encode('utf8') + sep + tail


def parse(data, pos, end, version, out):
    """Read the node records in [pos, end) into `out` as nested tuples."""
    fmt = '<QQQB' if version >= 7500 else '<IIIB'
    size = struct.calcsize(fmt)
    while pos + size <= end:
        stop, count, length, namelen = struct.unpack_from(fmt, data, pos)
        pos += size
        if stop == 0:          # the null record that terminates a nesting level
            out.append(None)
            return pos
        name = data[pos:pos + namelen]
        pos += namelen
        props = data[pos:pos + length]
        children = []
        after = pos + length
        if after < stop:
            parse(data, after, stop, version, children)
        out.append((name, count, props, children))
        pos = stop
    return pos


def properties(props, count):
    """Split a property block into (typecode, payload) pairs."""
    pos, fields = 0, []
    for _ in range(count):
        kind = props[pos:pos + 1]
        start = pos
        pos += 1
        code = kind.decode()
        if code in 'CB':
            pos += 1
        elif code == 'Y':
            pos += 2
        elif code in 'IF':
            pos += 4
        elif code in 'DL':
            pos += 8
        elif code in 'SR':
            pos += 4 + struct.unpack_from('<I', props, pos)[0]
        elif code in 'fdlib':
            _, _, compressed = struct.unpack_from('<III', props, pos)
            pos += 12 + compressed
        else:
            raise ValueError('unknown FBX property type %r' % code)
        fields.append((code, props[start:pos]))
    return fields


def convert_props(props, count):
    """Rewrite the name property of a Model node, leaving the rest verbatim."""
    fields = properties(props, count)
    # A Model's name is its second property, a string: `<name>\x00\x01Model`.
    if len(fields) < 2 or fields[1][0] != 'S':
        return props, 0
    payload = fields[1][1]
    value = payload[5:]
    renamed = rename(value)
    if renamed == value:
        return props, 0
    fields[1] = ('S', b'S' + struct.pack('<I', len(renamed)) + renamed)
    return b''.join(field for _, field in fields), 1


def convert(node, version, counter):
    if node is None:
        return None
    name, count, props, children = node
    if name == b'Model':
        props, renamed = convert_props(props, count)
        counter[0] += renamed
    return (name, count, props,
            [convert(child, version, counter) for child in children])


def write(nodes, version, out, offset):
    """Serialize nodes, back-patching each record's end offset."""
    fmt = '<QQQB' if version >= 7500 else '<IIIB'
    size = struct.calcsize(fmt)
    for node in nodes:
        if node is None:
            out.append(b'\x00' * size)
            offset += size
            continue
        name, count, props, children = node
        header = offset
        offset += size + len(name) + len(props)
        body = []
        if children:
            offset = write(children, version, body, offset)
        out.append(struct.pack(fmt, offset, count, len(props), len(name)))
        out.append(name)
        out.append(props)
        out.extend(body)
        del header
    return offset


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    source, destination = argv[1], argv[2]
    data = open(source, 'rb').read()
    if not data.startswith(b'Kaydara FBX Binary'):
        sys.stderr.write('%s is not a binary FBX\n' % source)
        return 1
    version = struct.unpack_from('<I', data, 23)[0]

    nodes = []
    end = parse(data, 27, len(data), version, nodes)

    counter = [0]
    converted = [convert(node, version, counter) for node in nodes]

    body = []
    write(converted, version, body, 27)
    # Everything past the last record -- the null terminator, the footer id,
    # padding and the file magic -- is version-specific and carries no names,
    # so it is copied through byte for byte.
    out = data[:27] + b''.join(body) + data[end:]
    open(destination, 'wb').write(out)
    print('%s -> %s  (%d bones renamed, %d -> %d bytes)'
          % (source, destination, counter[0], len(data), len(out)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
