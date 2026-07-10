struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID) {
    float2 position = vertexID == 0 ? float2(-1.0, -1.0)
                    : vertexID == 1 ? float2(-1.0,  3.0)
                                    : float2( 3.0, -1.0);
    VSOutput output;
    output.position = float4(position, 0.9999, 1.0);
    output.uv = position * float2(0.5, -0.5) + 0.5;
    return output;
}
