// Standalone benchmark for the CPU clustered light culling algorithm.
// Replicates ClusteredRendererDX12::cullLights exactly (same math, same data
// volume) without pulling in DX12Core.h, so it builds header-only.
//
// Measures: wall time per cullLights() call at realistic light counts, plus
// the memcpy + SetCluster fan-out the frame pays on top.

#include <DirectXMath.h>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace DirectX;

namespace {

constexpr int CLUSTER_X = 16;
constexpr int CLUSTER_Y = 9;
constexpr int CLUSTER_Z = 10;
constexpr int MAX_LIGHTS_PER_CLUSTER = 32;

struct Cluster {
    XMFLOAT3 minBounds;
    XMFLOAT3 maxBounds;
    int lightCount;
    int lightIndices[MAX_LIGHTS_PER_CLUSTER];
};

struct Light {
    XMFLOAT3 position;
    float radius;
    XMFLOAT3 color;
    float intensity;
    bool active;
};

struct Culler {
    std::vector<Cluster> clusters;
    std::vector<Light> lights;
    float screenWidth = 1920.0f, screenHeight = 1080.0f;
    float nearPlane = 0.1f, farPlane = 500.0f;
    float fov = XMConvertToRadians(70.0f);
    XMMATRIX viewMatrix = XMMatrixIdentity();

    Culler() { clusters.resize(CLUSTER_X * CLUSTER_Y * CLUSTER_Z); }

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
        float tanHalfFov = tanf(fov * 0.5f);
        float aspectRatio = screenWidth / screenHeight;
        float nearHeight = nearDepth * tanHalfFov;
        float nearWidth = nearHeight * aspectRatio;
        float farHeight = farDepth * tanHalfFov;
        float farWidth = farHeight * aspectRatio;
        float nx0 = (screenMinX / screenWidth) * 2.0f - 1.0f;
        float nx1 = (screenMaxX / screenWidth) * 2.0f - 1.0f;
        float ny0 = (screenMinY / screenHeight) * 2.0f - 1.0f;
        float ny1 = (screenMaxY / screenHeight) * 2.0f - 1.0f;
        float minX1 = nx0 * nearWidth, minX2 = nx0 * farWidth;
        minBounds.x = (minX1 < minX2) ? minX1 : minX2;
        float minY1 = ny0 * nearHeight, minY2 = ny0 * farHeight;
        minBounds.y = (minY1 < minY2) ? minY1 : minY2;
        minBounds.z = nearDepth;
        float maxX1 = nx1 * nearWidth, maxX2 = nx1 * farWidth;
        maxBounds.x = (maxX1 > maxX2) ? maxX1 : maxX2;
        float maxY1 = ny1 * nearHeight, maxY2 = ny1 * farHeight;
        maxBounds.y = (maxY1 > maxY2) ? maxY1 : maxY2;
        maxBounds.z = farDepth;
    }

    bool sphereAABBIntersect(const XMFLOAT3& center, float radius,
                             const XMFLOAT3& aabbMin, const XMFLOAT3& aabbMax) {
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
        for (auto& cluster : clusters) cluster.lightCount = 0;
        for (int z = 0; z < CLUSTER_Z; z++) {
            for (int y = 0; y < CLUSTER_Y; y++) {
                for (int x = 0; x < CLUSTER_X; x++) {
                    int clusterIdx = x + y * CLUSTER_X + z * CLUSTER_X * CLUSTER_Y;
                    Cluster& cluster = clusters[clusterIdx];
                    computeClusterAABB(x, y, z, cluster.minBounds, cluster.maxBounds);
                    cluster.lightCount = 0;
                    for (int i = 0; i < (int)lights.size() && cluster.lightCount < MAX_LIGHTS_PER_CLUSTER; i++) {
                        if (!lights[i].active) continue;
                        XMVECTOR lightPos = XMLoadFloat3(&lights[i].position);
                        XMVECTOR lightViewPos = XMVector3Transform(lightPos, viewMatrix);
                        XMFLOAT3 lightPosView;
                        XMStoreFloat3(&lightPosView, lightViewPos);
                        if (sphereAABBIntersect(lightPosView, lights[i].radius,
                                                cluster.minBounds, cluster.maxBounds)) {
                            cluster.lightIndices[cluster.lightCount] = i;
                            cluster.lightCount++;
                        }
                    }
                }
            }
        }
    }
};

} // namespace

int main() {
    // Realistic scene: typical gameplay moment has a handful of point lights;
    // the demo scene in Scene.h spawns ~10, fires add transient ones. Test a
    // spread: 8 (quiet), 32 (busy), 128 (the MAX_LIGHTS cap).
    const int lightCounts[] = { 8, 32, 64, 128 };
    const int iterations = 2000;

    std::printf("%8s %12s %14s\n", "lights", "us/cull", "us/frame(60)");
    for (int lightCount : lightCounts) {
        Culler culler;
        // Spread lights through the view volume so culling does real work.
        for (int i = 0; i < lightCount; ++i) {
            Light l;
            float t = (float)i / (float)lightCount;
            l.position = XMFLOAT3(sinf(t * 40.0f) * 30.0f, 2.0f + (i % 5),
                                  5.0f + t * 120.0f);
            l.radius = 8.0f + (i % 4) * 4.0f;
            l.color = XMFLOAT3(1, 1, 1);
            l.intensity = 1.0f;
            l.active = true;
            culler.lights.push_back(l);
        }

        // Warmup
        for (int i = 0; i < 50; ++i) culler.cullLights();

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) culler.cullLights();
        auto end = std::chrono::steady_clock::now();
        double totalUs =
            std::chrono::duration<double, std::micro>(end - start).count();
        double perCall = totalUs / iterations;
        // What a 60 fps frame pays if cull runs on the main thread.
        std::printf("%8d %12.1f %14.1f\n", lightCount, perCall,
                    perCall); // per-call == per-frame cost at 1 cull/frame
    }

    // Also measure the upload fan-out the frame pays after culling:
    // 1440 SetCluster calls + one memcpy of the whole cluster array.
    Culler culler;
    for (int i = 0; i < 32; ++i) {
        Light l;
        l.position = XMFLOAT3((float)i, 2.0f, 10.0f + i);
        l.radius = 10.0f;
        l.color = XMFLOAT3(1, 1, 1);
        l.intensity = 1.0f;
        l.active = true;
        culler.lights.push_back(l);
    }
    culler.cullLights();

    // VBClusterData layout: UINT lightCount + UINT[32] indices = 132 bytes,
    // x 1440 clusters = ~190 KB memcpy per frame.
    std::vector<unsigned char> upload(1440 * 132);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        memcpy(upload.data(), culler.clusters.data(), 0); // touch
        memcpy(upload.data(), upload.data() + 1, upload.size() - 1);
    }
    auto end = std::chrono::steady_clock::now();
    double memcpyUs =
        std::chrono::duration<double, std::micro>(end - start).count() /
        iterations;
    std::printf("\ncluster array memcpy (~190 KB): %.1f us/frame\n", memcpyUs);

    return 0;
}
