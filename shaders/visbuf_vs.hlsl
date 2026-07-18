// Visibility Buffer - Vertex Shader
// Transforms vertices to clip space and passes through draw/instance IDs

cbuffer MatrixBuffer : register(b0) {
    matrix unusedModel;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

cbuffer VisBufferConstants : register(b1) {
    uint drawCallID;
    uint alphaCutout;
    uint alphaFromLuminance;
    uint vbPadding2;
};

struct DrawCallData {
    float4x4 modelMatrix;
    float4x4 previousModelMatrix;
    float3 objectColor;
    float useTexture;
    float metalness;
    float roughness;
    float useNormalMap;
    uint materialID;
    uint vertexOffset;
    uint indexOffset;
    uint indexCount;
    uint hasIndices;
    uint flags;
};

StructuredBuffer<DrawCallData> drawCalls : register(t1);

struct VS_INPUT {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texCoord : TEXCOORD;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    
    float4 worldPos = mul(float4(input.position, 1.0),
                          drawCalls[drawCallID].modelMatrix);
    float4 viewPos  = mul(worldPos, view);
    output.position = mul(viewPos, projection);
    output.texCoord = input.texCoord;
    
    return output;
}
