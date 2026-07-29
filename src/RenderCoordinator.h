#ifndef RENDER_COORDINATOR_H
#define RENDER_COORDINATOR_H

enum class RenderPath {
    Forward,
    VisibilityBuffer,
    Raytracing
};

struct RenderPathRequest {
    bool requestRaytracing = false;
    bool requestVisibilityBuffer = false;
    bool raytracingAvailable = false;
    bool visibilityBufferAvailable = false;
};

class RenderCoordinator {
public:
    static RenderPath Choose(const RenderPathRequest& request) {
        if (request.requestRaytracing && request.raytracingAvailable)
            return RenderPath::Raytracing;
        if (request.requestVisibilityBuffer &&
            request.visibilityBufferAvailable)
            return RenderPath::VisibilityBuffer;
        return RenderPath::Forward;
    }
};

#endif
