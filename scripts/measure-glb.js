// Reads a GLB's JSON chunk and reports world-space bounds, triangle count and
// material flags. Used by Add-Prefab.ps1 so prefab fields are measured out of
// the file instead of assumed; prints one JSON object on stdout.
//
// Bounds come from each primitive's POSITION accessor min/max pushed through the
// node transforms, so a model whose scale sits on the node is reported at the
// size it actually appears in the world, not its raw mesh size.
const fs = require('fs');

function fail(message) {
  process.stdout.write(JSON.stringify({ error: message }));
  process.exit(1);
}

const path = process.argv[2];
if (!path) fail('usage: measure-glb.js <model.glb>');

let gltf;
let binary = null;
try {
  const fd = fs.openSync(path, 'r');
  const header = Buffer.alloc(12);
  fs.readSync(fd, header, 0, 12, 0);
  if (header.toString('utf8', 0, 4) !== 'glTF') {
    fs.closeSync(fd);
    fail('not a binary glTF (.glb) file');
  }
  const chunkHeader = Buffer.alloc(8);
  fs.readSync(fd, chunkHeader, 0, 8, 12);
  const chunkLength = chunkHeader.readUInt32LE(0);
  if (chunkHeader.toString('utf8', 4, 8) !== 'JSON') {
    fs.closeSync(fd);
    fail('first GLB chunk is not JSON');
  }
  const chunk = Buffer.alloc(chunkLength);
  fs.readSync(fd, chunk, 0, chunkLength, 20);
  gltf = JSON.parse(chunk.toString('utf8'));

  // The BIN chunk follows the JSON one. It is optional, and the open-shell test
  // below is simply skipped when it is absent.
  const binHeader = Buffer.alloc(8);
  const binStart = 20 + chunkLength;
  if (fs.readSync(fd, binHeader, 0, 8, binStart) === 8 &&
      binHeader.toString('utf8', 4, 8).startsWith('BIN')) {
    const binLength = binHeader.readUInt32LE(0);
    binary = Buffer.alloc(binLength);
    fs.readSync(fd, binary, 0, binLength, binStart + 8);
  }
  fs.closeSync(fd);
} catch (error) {
  fail(String(error && error.message ? error.message : error));
}

const IDENTITY = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];

function multiply(a, b) {
  const out = new Array(16);
  for (let c = 0; c < 4; c++)
    for (let r = 0; r < 4; r++) {
      let sum = 0;
      for (let k = 0; k < 4; k++) sum += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = sum;
    }
  return out;
}

// A node carries either a full matrix or a TRS triple; both collapse to the
// same column-major matrix here.
function localMatrix(node) {
  if (node.matrix) return node.matrix.slice();
  const t = node.translation || [0, 0, 0];
  const r = node.rotation || [0, 0, 0, 1];
  const s = node.scale || [1, 1, 1];
  const [x, y, z, w] = r;
  return [
    (1 - 2 * (y * y + z * z)) * s[0], (2 * (x * y + z * w)) * s[0], (2 * (x * z - y * w)) * s[0], 0,
    (2 * (x * y - z * w)) * s[1], (1 - 2 * (x * x + z * z)) * s[1], (2 * (y * z + x * w)) * s[1], 0,
    (2 * (x * z + y * w)) * s[2], (2 * (y * z - x * w)) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
    t[0], t[1], t[2], 1,
  ];
}

function transform(m, v) {
  return [
    m[0] * v[0] + m[4] * v[1] + m[8]  * v[2] + m[12],
    m[1] * v[0] + m[5] * v[1] + m[9]  * v[2] + m[13],
    m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14],
  ];
}

const nodes = gltf.nodes || [];
const meshes = gltf.meshes || [];
const accessors = gltf.accessors || [];
const materials = gltf.materials || [];

const min = [Infinity, Infinity, Infinity];
const max = [-Infinity, -Infinity, -Infinity];
let triangles = 0;
let primitives = 0;
const usedMaterials = new Set();
const TRIANGLE_MODE = 4;

// --- Accessor reading, for the open-shell test ---
const COMPONENT_READERS = {
  5120: [1, (b, o) => b.readInt8(o)],
  5121: [1, (b, o) => b.readUInt8(o)],
  5122: [2, (b, o) => b.readInt16LE(o)],
  5123: [2, (b, o) => b.readUInt16LE(o)],
  5125: [4, (b, o) => b.readUInt32LE(o)],
  5126: [4, (b, o) => b.readFloatLE(o)],
};
const COMPONENT_COUNTS = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 };

function readAccessor(accessorIndex) {
  const accessor = accessors[accessorIndex];
  if (!accessor || accessor.bufferView === undefined || !binary) return null;
  const view = (gltf.bufferViews || [])[accessor.bufferView];
  const reader = COMPONENT_READERS[accessor.componentType];
  const count = COMPONENT_COUNTS[accessor.type];
  if (!view || !reader || !count) return null;
  const [size, read] = reader;
  const stride = view.byteStride || size * count;
  const base = (view.byteOffset || 0) + (accessor.byteOffset || 0);
  if (base + (accessor.count - 1) * stride + count * size > binary.length) return null;
  const out = [];
  for (let i = 0; i < accessor.count; i++) {
    const row = [];
    for (let c = 0; c < count; c++) row.push(read(binary, base + i * stride + c * size));
    out.push(count === 1 ? row[0] : row);
  }
  return out;
}

// An open shell -- geometry with border edges rather than a sealed volume -- is
// what forceDoubleSided exists for: back-face culling makes it vanish when seen
// from the missing side. A closed solid has every edge shared by exactly two
// triangles, so counting edges used only once separates the two cases. Vertices
// are welded by rounded position first, since an exporter splits them per UV
// seam and per material and would otherwise make every mesh look open.
let boundaryEdges = 0;
let sharedEdges = 0;
let edgeTestRan = false;

function accumulateOpenShell(primitive) {
  if (!binary) return;
  const attributes = primitive.attributes || {};
  if (attributes.POSITION === undefined || primitive.indices === undefined) return;
  const positions = readAccessor(attributes.POSITION);
  const indices = readAccessor(primitive.indices);
  if (!positions || !indices) return;

  const welded = new Map();
  const representative = new Array(positions.length);
  for (let i = 0; i < positions.length; i++) {
    const p = positions[i];
    const key = `${Math.round(p[0] * 1000)},${Math.round(p[1] * 1000)},${Math.round(p[2] * 1000)}`;
    if (!welded.has(key)) welded.set(key, welded.size);
    representative[i] = welded.get(key);
  }

  const uses = new Map();
  for (let t = 0; t + 2 < indices.length; t += 3) {
    const corners = [
      representative[indices[t]],
      representative[indices[t + 1]],
      representative[indices[t + 2]],
    ];
    for (let e = 0; e < 3; e++) {
      const a = corners[e];
      const b = corners[(e + 1) % 3];
      if (a === b) continue;                 // degenerate edge
      const key = a < b ? `${a}_${b}` : `${b}_${a}`;
      uses.set(key, (uses.get(key) || 0) + 1);
    }
  }
  for (const count of uses.values()) {
    if (count === 1) boundaryEdges++;
    else sharedEdges++;
  }
  edgeTestRan = true;
}

const visited = new Set();
function visit(index, parent) {
  // glTF permits a malformed file to point a node at itself; without this a
  // cycle would spin here forever instead of reporting a size.
  if (visited.has(index)) return;
  visited.add(index);
  const node = nodes[index];
  if (!node) return;
  const world = multiply(parent, localMatrix(node));
  if (node.mesh !== undefined && meshes[node.mesh]) {
    for (const primitive of meshes[node.mesh].primitives || []) {
      primitives++;
      if (primitive.material !== undefined) usedMaterials.add(primitive.material);
      const mode = primitive.mode === undefined ? TRIANGLE_MODE : primitive.mode;
      const positions = accessors[(primitive.attributes || {}).POSITION];
      if (mode === TRIANGLE_MODE) {
        if (primitive.indices !== undefined && accessors[primitive.indices])
          triangles += accessors[primitive.indices].count / 3;
        else if (positions) triangles += positions.count / 3;
        accumulateOpenShell(primitive);
      }
      // A sparse or extension-packed accessor can omit min/max; such a
      // primitive contributes nothing to bounds rather than poisoning them.
      if (positions && positions.min && positions.max) {
        for (let corner = 0; corner < 8; corner++) {
          const local = [
            (corner & 1) ? positions.max[0] : positions.min[0],
            (corner & 2) ? positions.max[1] : positions.min[1],
            (corner & 4) ? positions.max[2] : positions.min[2],
          ];
          const world_ = transform(world, local);
          for (let axis = 0; axis < 3; axis++) {
            if (world_[axis] < min[axis]) min[axis] = world_[axis];
            if (world_[axis] > max[axis]) max[axis] = world_[axis];
          }
        }
      }
    }
  }
  for (const child of node.children || []) visit(child, world);
  visited.delete(index);
}

const scene = (gltf.scenes || [])[gltf.scene || 0];
const roots = scene && scene.nodes ? scene.nodes : nodes.map((_, i) => i);
for (const root of roots) visit(root, IDENTITY);

if (!isFinite(min[0])) fail('model has no positioned geometry to measure');

const used = [...usedMaterials].map((i) => materials[i]).filter(Boolean);
const materialList = used.map((m) => ({
  name: m.name || '',
  doubleSided: !!m.doubleSided,
  alphaMode: m.alphaMode || 'OPAQUE',
}));

process.stdout.write(JSON.stringify({
  size: [max[0] - min[0], max[1] - min[1], max[2] - min[2]],
  min,
  max,
  triangles: Math.round(triangles),
  primitives,
  materialsUsed: usedMaterials.size,
  images: (gltf.images || []).length,
  anyDoubleSided: materialList.some((m) => m.doubleSided),
  // Every-material double-sidedness is what separates "the art already handles
  // this" from "one sheet material was left single-sided". An empty list is not
  // uniformly double-sided.
  allDoubleSided: materialList.length > 0 && materialList.every((m) => m.doubleSided),
  anyAlphaNonOpaque: materialList.some((m) => m.alphaMode !== 'OPAQUE'),
  // Border-edge evidence for the open-shell question. openShell is null when the
  // test could not run (no BIN chunk, unindexed geometry) so the caller can tell
  // "measured closed" apart from "not measured".
  openShell: edgeTestRan ? boundaryEdges > 0 : null,
  boundaryEdges,
  sharedEdges,
  materials: materialList,
}));
