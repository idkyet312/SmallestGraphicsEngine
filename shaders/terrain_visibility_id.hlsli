#ifndef SGE_TERRAIN_VISIBILITY_ID_HLSLI
#define SGE_TERRAIN_VISIBILITY_ID_HLSLI

#define VB_TERRAIN_ID 0xFFFFFFFFu

// Mesh draw IDs occupy the low half. Relief terrain uses the high bit plus
// the original [0,1] device-depth bits; .y retains the full geometric normal.
// The resolve needs that original point to avoid applying POM twice.
bool IsTerrainVisibilityID(uint id) {
    return id == VB_TERRAIN_ID || (id >= 0x80000000u && id <= 0xBF800000u);
}

float TerrainVisibilityBaseDepth(uint id, float rasterDepth) {
    return id == VB_TERRAIN_ID ? rasterDepth : asfloat(id & 0x7FFFFFFFu);
}

uint TerrainVisibilitySurfaceID(uint id) {
    return IsTerrainVisibilityID(id) ? VB_TERRAIN_ID : id;
}

#endif
