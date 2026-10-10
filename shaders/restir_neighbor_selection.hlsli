// Geometry-only neighbor selection from Junkins et al., HPG 2026:
// https://research.nvidia.com/labs/rtr/publication/junkins2026compatibility/
// Keep the selection stream separate from the light reservoir's RNG; light
// weights and visibility must never influence which geometry is selected.
#ifndef SGE_RESTIR_NEIGHBOR_SELECTION_HLSLI
#define SGE_RESTIR_NEIGHBOR_SELECTION_HLSLI

float ReSTIRNeighborCompatibility(float3 position, float3 normal,
                                  float3 neighborPosition,
                                  float3 neighborNormal,
                                  float distanceToCamera) {
    // Equations 14-15: Omega = 0.05 sr, beta = 8. World-space distance avoids
    // the depth-only heuristic's failure on oblique and intersecting surfaces.
    float scale = max(distanceToCamera * sqrt(0.05 / 3.14159265), 1e-4);
    float cosine = saturate(dot(normal, neighborNormal));
    float cosine2 = cosine * cosine;
    float cosine4 = cosine2 * cosine2;
    return exp(-length(position - neighborPosition) / scale) *
           cosine4 * cosine4;
}

struct ReSTIRNeighborReservoir {
    int2 pixel;
    float weightSum;
};

ReSTIRNeighborReservoir ReSTIREmptyNeighborReservoir() {
    ReSTIRNeighborReservoir r;
    r.pixel = int2(-1, -1);
    r.weightSum = 0.0;
    return r;
}

// A-Chao WRS for one donor. Geometry validity and reprojection are resolved
// before this update; the selected donor's light reservoir is loaded afterward.
// This also keeps a low-scoring donor available when no high-scoring one exists.
void ReSTIRSelectNeighbor(inout ReSTIRNeighborReservoir r, int2 pixel,
                          float compatibility, float xi) {
    if (!(compatibility > 0.0) || !isfinite(compatibility)) return;
    r.weightSum += compatibility;
    if (xi * r.weightSum < compatibility)
        r.pixel = pixel;
}

#endif
