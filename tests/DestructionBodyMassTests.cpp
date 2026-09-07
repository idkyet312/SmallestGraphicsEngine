#include <box3d/box3d.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

static bool Near(float a, float b) {
    return std::abs(a - b) <= 1e-4f * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
}
static bool Near(b3Vec3 a, b3Vec3 b) {
    return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}
static b3BodyId Create(b3WorldId world, int count, bool batch, b3BodyType type, double& milliseconds) {
    b3BodyDef bodyDef = b3DefaultBodyDef();
    bodyDef.type = type;
    bodyDef.position = { 3, 5, 7 };
    bodyDef.linearVelocity = { 1, 2, 3 };
    bodyDef.angularVelocity = { 0.2f, -0.3f, 0.4f };
    const auto start = std::chrono::steady_clock::now();
    b3BodyId body = b3CreateBody(world, &bodyDef);
    b3ShapeDef shapeDef = b3DefaultShapeDef();
    shapeDef.density = type == b3_dynamicBody ? 8.0f : 0.0f;
    shapeDef.updateBodyMass = !batch;
    for (int i = 0; i < count; ++i) {
        const b3Vec3 offset{ float(i % 8), float((i / 8) % 8), float(i / 64) };
        b3BoxHull box = b3MakeOffsetBoxHull(0.2f + float(i % 3) * 0.05f, 0.3f, 0.4f, offset);
        b3CreateHullShape(body, &shapeDef, &box.base);
    }
    if (batch) b3Body_ApplyMassFromShapes(body);
    milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return body;
}

int main() {
    b3WorldDef definition = b3DefaultWorldDef();
    definition.workerCount = 1;
    b3WorldId world = b3CreateWorld(&definition);
    bool passed = true;
    for (b3BodyType type : { b3_staticBody, b3_dynamicBody }) {
        for (int count : { 1, 8, 64, 512 }) {
            double oldMs, newMs;
            b3BodyId oldBody = Create(world, count, false, type, oldMs);
            b3BodyId newBody = Create(world, count, true, type, newMs);
            const b3MassData a = b3Body_GetMassData(oldBody), b = b3Body_GetMassData(newBody);
            passed &= Near(a.mass, b.mass) && Near(a.center, b.center) &&
                Near(a.inertia.cx, b.inertia.cx) && Near(a.inertia.cy, b.inertia.cy) && Near(a.inertia.cz, b.inertia.cz);
            passed &= Near(b3Body_GetLinearVelocity(oldBody), b3Body_GetLinearVelocity(newBody));
            passed &= Near(b3Body_GetAngularVelocity(oldBody), b3Body_GetAngularVelocity(newBody));
            std::cout << "type=" << type << " shapes=" << count << " creation ms: " << oldMs << " -> " << newMs << '\n';
            b3DestroyBody(oldBody);
            b3DestroyBody(newBody);
        }
    }
    b3DestroyWorld(world);
    if (!passed) std::cerr << "Batched mass calculation changed body properties\n";
    return passed ? 0 : 1;
}
