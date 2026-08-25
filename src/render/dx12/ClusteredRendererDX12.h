#ifndef CLUSTERED_RENDERER_DX12_H
#define CLUSTERED_RENDERER_DX12_H

#include "DX12Core.h"
#include "ShaderDX12.h"  // For PointLightDataDX12
#include <DirectXMath.h>
#include <vector>
#include <algorithm>
#include <cmath>

using namespace DirectX;

extern DX12Context g_dx12;

// PointLightDataDX12 is defined in ShaderDX12.h

struct PointLightDX12 {
    XMFLOAT3 position;
    float radius;
    XMFLOAT3 color;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    bool active;
    // Spotlight cone, carried through to PointLightDataDX12 on upload. A zero
    // direction is an ordinary omnidirectional light, which is what the default
    // constructor leaves behind. Culling treats a spot as its bounding sphere:
    // conservative, and the shader's cone test rejects the rest per pixel.
    XMFLOAT3 spotDirection;
    float spotCosInner;
    float spotCosOuter;
    // Slice of the spot shadow atlas this light samples, or -1 for an ordinary
    // point light or unshadowed spot. The Humvee light owns its slice because
    // perspective depth has to be captured from that lamp's exact origin.
    int spotShadowIndex;
    // Whether this light draws a shaft in the volumetric fog. Surfaces are lit
    // either way -- this only controls the glow in the air.
    //
    // The fog multiplies the light's intensity directly, so a light whose
    // intensity is scaled for reach (the helicopter searchlight compensates
    // distance falloff up to 28) blooms in the air even though the surface it
    // is aimed at looks right. Turning the shaft off keeps that reach without
    // the bloom.
    bool volumetric;

    PointLightDX12() : position(XMFLOAT3(0,0,0)), radius(10.0f), color(XMFLOAT3(1,1,1)), intensity(1.0f),
                       constant(1.0f), linear(0.09f), quadratic(0.032f), active(true),
                       spotDirection(XMFLOAT3(0,0,0)), spotCosInner(0.0f), spotCosOuter(0.0f),
                       spotShadowIndex(-1), volumetric(true) {}
};

class ClusteredRendererDX12 {
public:
    static const int CLUSTER_X = 16;
    static const int CLUSTER_Y = 9;
    static const int CLUSTER_Z = 10;
    static const int MAX_LIGHTS_PER_CLUSTER = 32;
    static const int MAX_LIGHTS = 128;
    
    struct Cluster {
        XMFLOAT3 minBounds;
        XMFLOAT3 maxBounds;
        int lightCount;
        int lightIndices[MAX_LIGHTS_PER_CLUSTER];
    };
    
    std::vector<Cluster> clusters;
    std::vector<PointLightDX12> lights;
    
    float screenWidth, screenHeight;
    float nearPlane, farPlane;
    float fov;
    XMMATRIX viewMatrix;
    XMMATRIX projMatrix;
    XMMATRIX invProjMatrix;
    
    bool initialized = false;
    bool useClusteredRendering = true;
    bool showClusterDebug = false;
    
    ClusteredRendererDX12() {
        clusters.resize(CLUSTER_X * CLUSTER_Y * CLUSTER_Z);
        lights.reserve(MAX_LIGHTS);
    }
    
    ~ClusteredRendererDX12() {
        cleanup();
    }
    
    void init() {
        if (initialized) return;
        initialized = true;
    }
    
    void cleanup() {
        initialized = false;
    }
    
    void setScreenSize(float width, float height) {
        screenWidth = width;
        screenHeight = height;
    }
    
    void setCamera(float fovDegrees, float nearVal, float farVal, const XMMATRIX& view, const XMMATRIX& proj) {
        fov = XMConvertToRadians(fovDegrees);
        nearPlane = nearVal;
        farPlane = farVal;
        viewMatrix = view;
        projMatrix = proj;
        invProjMatrix = XMMatrixInverse(nullptr, proj);
    }
    
    int addLight(const XMFLOAT3& position, const XMFLOAT3& color, float radius, float intensity = 1.0f) {
        if (lights.size() >= MAX_LIGHTS) return -1;
        
        PointLightDX12 light;
        light.position = position;
        light.color = color;
        light.radius = radius;
        light.intensity = intensity;
        light.active = true;
        light.constant = 1.0f;
        light.linear = 4.5f / radius;
        light.quadratic = 75.0f / (radius * radius);
        
        lights.push_back(light);
        return (int)lights.size() - 1;
    }
    
    void removeLight(int index) {
        if (index >= 0 && index < (int)lights.size()) {
            lights[index].active = false;
        }
    }
    
    void clearLights() {
        lights.clear();
    }
    
    void updateLight(int index, const XMFLOAT3& position) {
        if (index >= 0 && index < (int)lights.size()) {
            lights[index].position = position;
        }
    }
    
    void updateLight(int index, const XMFLOAT3& position, const XMFLOAT3& color, float radius, float intensity) {
        if (index >= 0 && index < (int)lights.size()) {
            lights[index].position = position;
            lights[index].color = color;
            lights[index].radius = radius;
            lights[index].intensity = intensity;
            lights[index].linear = 4.5f / radius;
            lights[index].quadratic = 75.0f / (radius * radius);
        }
    }
    
    int getDepthSlice(float depth) {
        float logDepth = logf(depth / nearPlane) / logf(farPlane / nearPlane);
        int slice = (int)(logDepth * CLUSTER_Z);
        if (slice < 0) slice = 0;
        if (slice >= CLUSTER_Z) slice = CLUSTER_Z - 1;
        return slice;
    }
    
    float getDepthFromSlice(int slice) {
        float t = (float)slice / (float)CLUSTER_Z;
        return nearPlane * powf(farPlane / nearPlane, t);
    }
    
    void computeClusterAABB(int x, int y, int z, XMFLOAT3& minBounds, XMFLOAT3& maxBounds) {
        float tileWidth = screenWidth / CLUSTER_X;
        float tileHeight = screenHeight / CLUSTER_Y;
        
        float screenMinX = x * tileWidth;
        float screenMaxX = (x + 1) * tileWidth;
        float screenMinY = y * tileHeight;
        float screenMaxY = (y + 1) * tileHeight;
        
        float nearDepth = getDepthFromSlice(z);
        float farDepth = getDepthFromSlice(z + 1);
        
        // Simple AABB calculation in view space
        float tanHalfFov = tanf(fov * 0.5f);
        float aspectRatio = screenWidth / screenHeight;
        
        float nearHeight = nearDepth * tanHalfFov;
        float nearWidth = nearHeight * aspectRatio;
        float farHeight = farDepth * tanHalfFov;
        float farWidth = farHeight * aspectRatio;
        
        // Convert screen space to normalized [-1, 1]
        float nx0 = (screenMinX / screenWidth) * 2.0f - 1.0f;
        float nx1 = (screenMaxX / screenWidth) * 2.0f - 1.0f;
        float ny0 = (screenMinY / screenHeight) * 2.0f - 1.0f;
        float ny1 = (screenMaxY / screenHeight) * 2.0f - 1.0f;
        
        // Calculate corners in view space
        float minX1 = nx0 * nearWidth;
        float minX2 = nx0 * farWidth;
        minBounds.x = (minX1 < minX2) ? minX1 : minX2;
        
        float minY1 = ny0 * nearHeight;
        float minY2 = ny0 * farHeight;
        minBounds.y = (minY1 < minY2) ? minY1 : minY2;
        minBounds.z = nearDepth;
        
        float maxX1 = nx1 * nearWidth;
        float maxX2 = nx1 * farWidth;
        maxBounds.x = (maxX1 > maxX2) ? maxX1 : maxX2;
        
        float maxY1 = ny1 * nearHeight;
        float maxY2 = ny1 * farHeight;
        maxBounds.y = (maxY1 > maxY2) ? maxY1 : maxY2;
        maxBounds.z = farDepth;
    }
    
    bool sphereAABBIntersect(const XMFLOAT3& center, float radius, const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax) {
        float closestX = center.x;
        if (closestX < aabbMin.x) closestX = aabbMin.x;
        if (closestX > aabbMax.x) closestX = aabbMax.x;
        
        float closestY = center.y;
        if (closestY < aabbMin.y) closestY = aabbMin.y;
        if (closestY > aabbMax.y) closestY = aabbMax.y;
        
        float closestZ = center.z;
        if (closestZ < aabbMin.z) closestZ = aabbMin.z;
        if (closestZ > aabbMax.z) closestZ = aabbMax.z;
        
        float dx = center.x - closestX;
        float dy = center.y - closestY;
        float dz = center.z - closestZ;
        float distSq = dx * dx + dy * dy + dz * dz;
        
        return distSq <= (radius * radius);
    }
    
    void cullLights() {
        if (!initialized) init();
        
        for (auto& cluster : clusters) {
            cluster.lightCount = 0;
        }
        
        for (int z = 0; z < CLUSTER_Z; z++) {
            for (int y = 0; y < CLUSTER_Y; y++) {
                for (int x = 0; x < CLUSTER_X; x++) {
                    int clusterIdx = x + y * CLUSTER_X + z * CLUSTER_X * CLUSTER_Y;
                    Cluster& cluster = clusters[clusterIdx];
                    
                    computeClusterAABB(x, y, z, cluster.minBounds, cluster.maxBounds);
                    cluster.lightCount = 0;
                    
                    for (int i = 0; i < (int)lights.size() && cluster.lightCount < MAX_LIGHTS_PER_CLUSTER; i++) {
                        if (!lights[i].active) continue;
                        
                        // Transform light position to view space
                        XMVECTOR lightPos = XMLoadFloat3(&lights[i].position);
                        XMVECTOR lightViewPos = XMVector3Transform(lightPos, viewMatrix);
                        XMFLOAT3 lightPosView;
                        XMStoreFloat3(&lightPosView, lightViewPos);
                        
                        if (sphereAABBIntersect(lightPosView, lights[i].radius, cluster.minBounds, cluster.maxBounds)) {
                            cluster.lightIndices[cluster.lightCount] = i;
                            cluster.lightCount++;
                        }
                    }
                }
            }
        }
    }
    
    std::vector<PointLightDataDX12> getPointLightData() {
        std::vector<PointLightDataDX12> data;
        for (const auto& light : lights) {
            if (!light.active) continue;
            PointLightDataDX12 pld;
            pld.position = light.position;
            pld.radius = light.radius;
            pld.color = light.color;
            pld.intensity = light.intensity;
            pld.spotDirection = light.spotDirection;
            pld.spotCosInner = light.spotCosInner;
            pld.spotCosOuter = light.spotCosOuter;
            pld.spotShadowIndex = light.spotShadowIndex;
            data.push_back(pld);
        }
        return data;
    }
    
    int getLightCount() const { 
        int count = 0;
        for (const auto& light : lights) {
            if (light.active) count++;
        }
        return count;
    }
    
    int getTotalLightCount() const {
        return (int)lights.size();
    }
    
    int getActiveClusterCount() const {
        int count = 0;
        for (const auto& cluster : clusters) {
            if (cluster.lightCount > 0) count++;
        }
        return count;
    }
    
    // Get light at index
    PointLightDX12* getLight(int index) {
        if (index >= 0 && index < (int)lights.size()) {
            return &lights[index];
        }
        return nullptr;
    }
    
    // Get all active lights as a flat array for shader upload
    void getLightsForShader(std::vector<PointLightDataDX12>& outLights) {
        outLights.clear();
        for (const auto& light : lights) {
            if (!light.active) continue;
            PointLightDataDX12 pld;
            pld.position = light.position;
            pld.radius = light.radius;
            pld.color = light.color;
            pld.intensity = light.intensity;
            pld.spotDirection = light.spotDirection;
            pld.spotCosInner = light.spotCosInner;
            pld.spotCosOuter = light.spotCosOuter;
            pld.spotShadowIndex = light.spotShadowIndex;
            outLights.push_back(pld);
        }
    }
};

#endif // CLUSTERED_RENDERER_DX12_H
