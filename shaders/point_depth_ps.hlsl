// Point Light Depth Pixel Shader - outputs linear depth for cube shadow maps

cbuffer PointLightBuffer : register(b1) {
    float3 lightPosition;
    float lightRadius;
};

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

float main(PS_INPUT input) : SV_Depth {
    // Calculate linear distance from light to fragment
    float distance = length(input.worldPos - lightPosition);
    
    // Normalize by light radius to store in [0,1] range
    float normalizedDepth = distance / lightRadius;
    
    return saturate(normalizedDepth);
}

