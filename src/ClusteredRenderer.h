#ifndef CLUSTERED_RENDERER_H
#define CLUSTERED_RENDERER_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <algorithm>

// Clustered Forward Rendering Implementation
// Divides view frustum into 3D clusters for efficient light culling

struct PointLight {
    glm::vec3 position;
    float radius;
    glm::vec3 color;
    float intensity;
    // Attenuation
    float constant;
    float linear;
    float quadratic;
    bool active;
    
    PointLight() : position(0.0f), radius(10.0f), color(1.0f), intensity(1.0f),
                   constant(1.0f), linear(0.09f), quadratic(0.032f), active(true) {}
};

class ClusteredRenderer {
public:
    // Cluster grid dimensions
    static const int CLUSTER_X = 16;  // Tiles in X
    static const int CLUSTER_Y = 9;   // Tiles in Y  
    static const int CLUSTER_Z = 24;  // Depth slices
    static const int MAX_LIGHTS_PER_CLUSTER = 32;
    static const int MAX_LIGHTS = 128;
    
    // Cluster data
    struct Cluster {
        glm::vec3 minBounds;
        glm::vec3 maxBounds;
        int lightCount;
        int lightIndices[MAX_LIGHTS_PER_CLUSTER];
    };
    
    std::vector<Cluster> clusters;
    std::vector<PointLight> lights;
    
    // GPU resources
    unsigned int lightSSBO;          // Light data buffer
    unsigned int clusterSSBO;        // Cluster light indices buffer
    unsigned int lightGridTexture;   // 3D texture for cluster -> light mapping
    unsigned int lightIndexTexture;  // 1D texture for light index list
    
    // Screen/camera info
    float screenWidth, screenHeight;
    float nearPlane, farPlane;
    float fov;
    glm::mat4 viewMatrix;
    glm::mat4 projMatrix;
    glm::mat4 invProjMatrix;
    
    bool initialized = false;
    bool useClusteredRendering = true;
    bool showClusterDebug = false;
    
    ClusteredRenderer() {
        clusters.resize(CLUSTER_X * CLUSTER_Y * CLUSTER_Z);
        lights.reserve(MAX_LIGHTS);
    }
    
    ~ClusteredRenderer() {
        cleanup();
    }
    
    void init() {
        if (initialized) return;
        
        // Create light data texture (stores light properties)
        // Using a 1D texture to store light data (position, color, radius, etc.)
        glGenTextures(1, &lightSSBO);
        glBindTexture(GL_TEXTURE_1D, lightSSBO);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA32F, MAX_LIGHTS * 4, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        
        // Create 3D texture for cluster light grid
        // Each texel stores: (lightCount, startIndex, 0, 0)
        glGenTextures(1, &lightGridTexture);
        glBindTexture(GL_TEXTURE_3D, lightGridTexture);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RG32I, CLUSTER_X, CLUSTER_Y, CLUSTER_Z, 0, GL_RG_INTEGER, GL_INT, nullptr);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        
        // Create 1D texture for light index list
        // Stores indices of lights affecting each cluster
        int maxIndices = CLUSTER_X * CLUSTER_Y * CLUSTER_Z * MAX_LIGHTS_PER_CLUSTER;
        glGenTextures(1, &lightIndexTexture);
        glBindTexture(GL_TEXTURE_1D, lightIndexTexture);
        glTexImage1D(GL_TEXTURE_1D, 0, GL_R32I, maxIndices, 0, GL_RED_INTEGER, GL_INT, nullptr);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        
        initialized = true;
    }
    
    void cleanup() {
        if (initialized) {
            glDeleteTextures(1, &lightSSBO);
            glDeleteTextures(1, &lightGridTexture);
            glDeleteTextures(1, &lightIndexTexture);
            initialized = false;
        }
    }
    
    void setScreenSize(float width, float height) {
        screenWidth = width;
        screenHeight = height;
    }
    
    void setCamera(float fovDegrees, float near, float far, const glm::mat4& view, const glm::mat4& proj) {
        fov = glm::radians(fovDegrees);
        nearPlane = near;
        farPlane = far;
        viewMatrix = view;
        projMatrix = proj;
        invProjMatrix = glm::inverse(proj);
    }
    
    // Add a point light to the scene
    int addLight(const glm::vec3& position, const glm::vec3& color, float radius, float intensity = 1.0f) {
        if (lights.size() >= MAX_LIGHTS) return -1;
        
        PointLight light;
        light.position = position;
        light.color = color;
        light.radius = radius;
        light.intensity = intensity;
        light.active = true;
        
        // Calculate attenuation from radius
        // Using the formula: attenuation = 1/(c + l*d + q*d^2)
        // At distance = radius, we want attenuation to be ~0.01
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
    
    // Convert screen position + depth to view space
    glm::vec3 screenToView(const glm::vec4& screenPos) {
        // Convert to NDC
        glm::vec4 ndc;
        ndc.x = (screenPos.x / screenWidth) * 2.0f - 1.0f;
        ndc.y = (screenPos.y / screenHeight) * 2.0f - 1.0f;
        ndc.z = screenPos.z * 2.0f - 1.0f;
        ndc.w = 1.0f;
        
        // Transform by inverse projection
        glm::vec4 viewPos = invProjMatrix * ndc;
        viewPos /= viewPos.w;
        
        return glm::vec3(viewPos);
    }
    
    // Get depth slice from linear depth using exponential distribution
    int getDepthSlice(float depth) {
        // Logarithmic depth slicing for better distribution
        float logDepth = log(depth / nearPlane) / log(farPlane / nearPlane);
        int slice = (int)(logDepth * CLUSTER_Z);
        return glm::clamp(slice, 0, CLUSTER_Z - 1);
    }
    
    // Get linear depth from depth slice
    float getDepthFromSlice(int slice) {
        float t = (float)slice / (float)CLUSTER_Z;
        return nearPlane * pow(farPlane / nearPlane, t);
    }
    
    // Compute AABB for a cluster in view space
    void computeClusterAABB(int x, int y, int z, glm::vec3& minBounds, glm::vec3& maxBounds) {
        float tileWidth = screenWidth / CLUSTER_X;
        float tileHeight = screenHeight / CLUSTER_Y;
        
        // Screen space bounds
        float screenMinX = x * tileWidth;
        float screenMaxX = (x + 1) * tileWidth;
        float screenMinY = y * tileHeight;
        float screenMaxY = (y + 1) * tileHeight;
        
        // Depth bounds
        float nearDepth = getDepthFromSlice(z);
        float farDepth = getDepthFromSlice(z + 1);
        
        // Convert corners to view space
        glm::vec3 corners[8];
        corners[0] = screenToView(glm::vec4(screenMinX, screenMinY, 0, 1)) * nearDepth / nearPlane;
        corners[1] = screenToView(glm::vec4(screenMaxX, screenMinY, 0, 1)) * nearDepth / nearPlane;
        corners[2] = screenToView(glm::vec4(screenMinX, screenMaxY, 0, 1)) * nearDepth / nearPlane;
        corners[3] = screenToView(glm::vec4(screenMaxX, screenMaxY, 0, 1)) * nearDepth / nearPlane;
        corners[4] = screenToView(glm::vec4(screenMinX, screenMinY, 0, 1)) * farDepth / nearPlane;
        corners[5] = screenToView(glm::vec4(screenMaxX, screenMinY, 0, 1)) * farDepth / nearPlane;
        corners[6] = screenToView(glm::vec4(screenMinX, screenMaxY, 0, 1)) * farDepth / nearPlane;
        corners[7] = screenToView(glm::vec4(screenMaxX, screenMaxY, 0, 1)) * farDepth / nearPlane;
        
        // Compute AABB
        minBounds = corners[0];
        maxBounds = corners[0];
        for (int i = 1; i < 8; i++) {
            minBounds = glm::min(minBounds, corners[i]);
            maxBounds = glm::max(maxBounds, corners[i]);
        }
    }
    
    // Test if a sphere intersects an AABB
    bool sphereAABBIntersect(const glm::vec3& center, float radius, const glm::vec3& aabbMin, const glm::vec3& aabbMax) {
        glm::vec3 closestPoint = glm::clamp(center, aabbMin, aabbMax);
        float distSq = glm::dot(center - closestPoint, center - closestPoint);
        return distSq <= (radius * radius);
    }
    
    // Assign lights to clusters (CPU-side culling)
    void cullLights() {
        if (!initialized) init();
        
        // Reset cluster light counts
        for (auto& cluster : clusters) {
            cluster.lightCount = 0;
        }
        
        // Build light index list
        std::vector<int> lightIndexList;
        std::vector<glm::ivec2> clusterLightInfo(CLUSTER_X * CLUSTER_Y * CLUSTER_Z);
        
        // For each cluster
        for (int z = 0; z < CLUSTER_Z; z++) {
            for (int y = 0; y < CLUSTER_Y; y++) {
                for (int x = 0; x < CLUSTER_X; x++) {
                    int clusterIdx = x + y * CLUSTER_X + z * CLUSTER_X * CLUSTER_Y;
                    Cluster& cluster = clusters[clusterIdx];
                    
                    // Compute cluster AABB in view space
                    computeClusterAABB(x, y, z, cluster.minBounds, cluster.maxBounds);
                    
                    // Record start index for this cluster
                    int startIndex = (int)lightIndexList.size();
                    cluster.lightCount = 0;
                    
                    // Test each light against this cluster
                    for (int i = 0; i < (int)lights.size() && cluster.lightCount < MAX_LIGHTS_PER_CLUSTER; i++) {
                        if (!lights[i].active) continue;
                        
                        // Transform light position to view space
                        glm::vec4 lightViewPos = viewMatrix * glm::vec4(lights[i].position, 1.0f);
                        glm::vec3 lightPosView = glm::vec3(lightViewPos);
                        
                        // Test sphere-AABB intersection
                        if (sphereAABBIntersect(lightPosView, lights[i].radius, cluster.minBounds, cluster.maxBounds)) {
                            cluster.lightIndices[cluster.lightCount] = i;
                            cluster.lightCount++;
                            lightIndexList.push_back(i);
                        }
                    }
                    
                    // Store cluster info
                    clusterLightInfo[clusterIdx] = glm::ivec2(cluster.lightCount, startIndex);
                }
            }
        }
        
        // Upload light data to texture
        std::vector<glm::vec4> lightData(MAX_LIGHTS * 4, glm::vec4(0.0f));
        for (int i = 0; i < (int)lights.size(); i++) {
            if (!lights[i].active) continue;
            // 4 vec4s per light:
            // [0]: position.xyz, radius
            // [1]: color.rgb, intensity
            // [2]: constant, linear, quadratic, 0
            // [3]: reserved
            lightData[i * 4 + 0] = glm::vec4(lights[i].position, lights[i].radius);
            lightData[i * 4 + 1] = glm::vec4(lights[i].color, lights[i].intensity);
            lightData[i * 4 + 2] = glm::vec4(lights[i].constant, lights[i].linear, lights[i].quadratic, 0.0f);
            lightData[i * 4 + 3] = glm::vec4(0.0f);
        }
        
        glBindTexture(GL_TEXTURE_1D, lightSSBO);
        glTexSubImage1D(GL_TEXTURE_1D, 0, 0, MAX_LIGHTS * 4, GL_RGBA, GL_FLOAT, lightData.data());
        
        // Upload cluster grid to 3D texture
        glBindTexture(GL_TEXTURE_3D, lightGridTexture);
        glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, CLUSTER_X, CLUSTER_Y, CLUSTER_Z, GL_RG_INTEGER, GL_INT, clusterLightInfo.data());
        
        // Upload light index list to 1D texture
        if (!lightIndexList.empty()) {
            // Pad to at least 1 element
            glBindTexture(GL_TEXTURE_1D, lightIndexTexture);
            glTexSubImage1D(GL_TEXTURE_1D, 0, 0, (GLsizei)lightIndexList.size(), GL_RED_INTEGER, GL_INT, lightIndexList.data());
        }
    }
    
    // Bind textures for shader use
    void bindForRendering(unsigned int lightTexUnit, unsigned int gridTexUnit, unsigned int indexTexUnit) {
        glActiveTexture(GL_TEXTURE0 + lightTexUnit);
        glBindTexture(GL_TEXTURE_1D, lightSSBO);
        
        glActiveTexture(GL_TEXTURE0 + gridTexUnit);
        glBindTexture(GL_TEXTURE_3D, lightGridTexture);
        
        glActiveTexture(GL_TEXTURE0 + indexTexUnit);
        glBindTexture(GL_TEXTURE_1D, lightIndexTexture);
    }
    
    // Get cluster index from fragment position
    glm::ivec3 getClusterIndex(const glm::vec3& fragPos, float fragDepth) {
        int x = (int)((fragPos.x / screenWidth) * CLUSTER_X);
        int y = (int)((fragPos.y / screenHeight) * CLUSTER_Y);
        int z = getDepthSlice(fragDepth);
        
        x = glm::clamp(x, 0, CLUSTER_X - 1);
        y = glm::clamp(y, 0, CLUSTER_Y - 1);
        
        return glm::ivec3(x, y, z);
    }
    
    int getLightCount() const { return (int)lights.size(); }
    int getActiveClusterCount() const {
        int count = 0;
        for (const auto& cluster : clusters) {
            if (cluster.lightCount > 0) count++;
        }
        return count;
    }
};

#endif

