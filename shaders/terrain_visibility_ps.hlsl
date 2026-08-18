// Terrain visibility pass pixel shader.
//
// Terrain is procedural: it has no vertex/index buffers the resolve could index
// into, so the usual (drawCallID, primitiveID) pair cannot describe it. Instead
// this writes a reserved ID plus the packed geometric normal, which together
// with depth is everything the resolve needs to rebuild the surface --
// world position comes from depth, and the triplanar material is a pure
// function of world position and that normal.
//
// Deliberately minimal: no texture fetches, no lighting. All of that moves to
// the resolve, which runs once per visible pixel instead of once per rasterized
// fragment. Avoiding overdraw on the clipmap rings is the entire point.

// Matches terrain_ms.hlsl OutVertex.
struct PS_INPUT {
    float4 position          : SV_Position;
    float3 fragPos           : TEXCOORD0;
    float3 normal            : TEXCOORD1;
    float2 texCoord          : TEXCOORD2;
    float4 tangent           : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
};

// Octahedral normal encoding, 16 bits per axis packed into one uint. The
// terrain normal is a smooth finite difference of the height field, so this is
// far finer than the triplanar blend downstream can resolve.
uint PackTerrainNormal(float3 normal) {
    normal /= max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1e-6);
    float2 encoded = normal.z >= 0.0
        ? normal.xy
        : (1.0 - abs(normal.yx)) *
          float2(normal.x >= 0.0 ? 1.0 : -1.0,
                 normal.y >= 0.0 ? 1.0 : -1.0);
    uint2 quantized = (uint2)round(saturate(encoded * 0.5 + 0.5) * 65535.0);
    return quantized.x | (quantized.y << 16u);
}

uint2 main(PS_INPUT input) : SV_Target0 {
    // 0xFFFFFFFF is the reserved terrain ID. Zero stays background, and real
    // draw calls are stored as drawCallID + 1, so they can never reach it.
    return uint2(0xFFFFFFFFu, PackTerrainNormal(normalize(input.normal)));
}
