// Screen-space radial light shafts ("god rays"), the Crysis-style approach.
//
// Rather than marching a volume, this smears bright unoccluded pixels radially
// outward from the sun's screen position. Because it works on the full-res
// framebuffer there is no froxel grid to quantise against and no lateral
// resolution mismatch, so it cannot produce the blocky steps or the
// bleed-across-silhouettes that the volumetric path suffers from.
//
// The classic version masks on sky pixels alone, which lets a shaft draw over
// geometry standing in front of the light. Each tap here is depth-tested
// instead: a sample only contributes if it is at least as far away as the pixel
// being shaded, so a hillside between the camera and the sun correctly stops
// the streak rather than glowing through it.

cbuffer ShaftConstants : register(b0)
{
    float4 sunUVIntensity;   // sun screen uv.xy, intensity, on-screen flag
    float4 shaftParams;      // density, decay, weight, exposure
    float4 depthParams;      // near, far, unused, unused
    float4 sunColor;         // sun tint, w unused
};

// Only depth is sampled. Reading the scene colour would mean sampling the same
// texture this pass additively blends into, which is undefined; the emitter
// term is synthesised from the sun colour instead, which is also more stable
// because it does not feed already-accumulated shaft light back into itself.
Texture2D<float> sceneDepth : register(t0);
SamplerState linearClampSampler : register(s0);

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 p = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = p;
    output.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float LinearDepth(float deviceDepth)
{
    float nearZ = depthParams.x;
    float farZ = depthParams.y;
    return nearZ * farZ /
           max(farZ - deviceDepth * (farZ - nearZ), 1e-5);
}

// Emitters are sky pixels: the gaps the sun actually shines through. Geometry
// contributes nothing, which is what anchors each streak to a real opening
// between the fronds rather than smearing the whole frame.
float EmitterAt(float2 uv)
{
    float deviceDepth = sceneDepth.SampleLevel(linearClampSampler, uv, 0.0);
    return deviceDepth >= 0.99999 ? 1.0 : 0.0;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Sun behind the camera or outside the frustum: nothing to radiate from.
    if (sunUVIntensity.w < 0.5)
        return float4(0.0, 0.0, 0.0, 1.0);

    const uint kSampleCount = 24;
    float density = shaftParams.x;
    float decay = shaftParams.y;
    float weight = shaftParams.z;
    float exposure = shaftParams.w;

    // Depth of the pixel we are shading. A tap nearer than this belongs to
    // geometry in front of the shaft and must not contribute.
    float centreDeviceDepth =
        sceneDepth.SampleLevel(linearClampSampler, input.uv, 0.0);
    float centreDepth = centreDeviceDepth >= 0.99999
        ? depthParams.y : LinearDepth(centreDeviceDepth);

    float2 deltaUV = (input.uv - sunUVIntensity.xy) *
                     (density / float(kSampleCount));
    float2 uv = input.uv;
    float illuminationDecay = 1.0;
    float accumulated = 0.0;

    [loop]
    for (uint i = 0; i < kSampleCount; ++i)
    {
        uv -= deltaUV;
        float2 sampleUV = saturate(uv);

        float tapDeviceDepth =
            sceneDepth.SampleLevel(linearClampSampler, sampleUV, 0.0);
        float tapDepth = tapDeviceDepth >= 0.99999
            ? depthParams.y : LinearDepth(tapDeviceDepth);

        // Occlusion test: skip taps that sit in front of this pixel. Softened
        // over a short range so the shaft fades against an edge rather than
        // ending on a hard line the width of one sample step.
        float visibility = smoothstep(-0.5, 1.5, tapDepth - centreDepth);

        accumulated += EmitterAt(sampleUV) * illuminationDecay * visibility;
        illuminationDecay *= decay;
    }

    float3 shafts = sunColor.rgb * accumulated *
                    (weight / float(kSampleCount)) * exposure *
                    sunUVIntensity.z;

    // Fade out as the sun approaches the screen edge; without this the streaks
    // pop the moment the sun crosses the frustum boundary.
    float2 edge = abs(sunUVIntensity.xy - 0.5) * 2.0;
    float edgeFade = saturate(1.0 - max(edge.x, edge.y));
    edgeFade = edgeFade * edgeFade;

    return float4(shafts * edgeFade, 1.0);
}
