#ifndef CAMERA_DX12_H
#define CAMERA_DX12_H

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

class Camera {
public:
    XMFLOAT3 Position;
    XMFLOAT3 Front;
    XMFLOAT3 Up;
    float Yaw;
    float Pitch;
    float MovementSpeed;
    float MouseSensitivity;
    
    // FPS mode settings
    bool FPSMode;
    float PlayerHeight;
    float FloorY;
    bool IsCrouching;
    bool IsSliding;
    XMFLOAT3 SlideDirection;
    float SlideSpeed;
    float SlideTimeRemaining;
    
    // Jump mechanics
    bool IsGrounded;
    float VerticalVelocity;
    float Gravity;
    float JumpStrength;

    // Swimming. WaterSurfaceY is pushed in each frame by the caller, which owns
    // the ocean; the camera only compares against it. NoWater parks the surface
    // below every level so the check is a no-op until someone sets it.
    static constexpr float NoWater = -1e9f;
    float WaterSurfaceY = NoWater;
    bool IsSwimming = false;
    // Vertical swim input for this frame: +1 rising, -1 diving, 0 coasting.
    float SwimInput = 0.0f;

    // Swim tuning. Water this shallow is waded, not swum -- as a fraction of
    // player height, so crouching does not change where the beach becomes sea.
    static constexpr float kWadeDepthFraction = 0.75f;
    // How far the eyes ride below the surface while treading water. Small, so
    // the waterline sits at chin height and the view stays clear.
    static constexpr float kEyesUnderSurface = 0.22f;
    static constexpr float kBuoyancyStiffness = 7.0f;   // 1/s^2 toward float line
    // Gentler restoring pull once the head clears the surface -- a body out of
    // the water is not being buoyed, it is just falling back to the line.
    static constexpr float kAboveSurfaceBuoyancyScale = 0.55f;
    static constexpr float kSwimVerticalAccel = 9.0f;   // m/s^2 from Space/Q
    static constexpr float kWaterDrag = 3.4f;           // 1/s velocity decay
    static constexpr float kMaxSwimSpeed = 3.2f;        // m/s vertical clamp
    // Swimming is slower than running, and the stroke carries you level rather
    // than letting the look direction drive depth.
    static constexpr float kSwimSpeedScale = 0.55f;

    Camera(XMFLOAT3 position = XMFLOAT3(0.0f, 5.0f, 10.0f))
        : Position(position), Front(XMFLOAT3(0.0f, 0.0f, -1.0f)), Up(XMFLOAT3(0.0f, 1.0f, 0.0f)),
          Yaw(-90.0f), Pitch(-5.0f), MovementSpeed(5.0f), MouseSensitivity(0.1f),
          FPSMode(true), PlayerHeight(1.7f), FloorY(0.0f),
          IsCrouching(false), IsSliding(false),
          SlideDirection(0.0f, 0.0f, -1.0f), SlideSpeed(0.0f),
          SlideTimeRemaining(0.0f),
          IsGrounded(true), VerticalVelocity(0.0f), Gravity(9.8f), JumpStrength(5.0f) {
        updateCameraVectors();
    }

    XMMATRIX GetViewMatrix() {
        XMVECTOR pos = XMLoadFloat3(&Position);
        XMVECTOR front = XMLoadFloat3(&Front);
        XMVECTOR up = XMLoadFloat3(&Up);
        if (explosionTrauma_ > 0.001f) {
            const float strength = explosionTrauma_ * explosionTrauma_;
            const float yaw = std::sin(explosionShakeTime_ * 37.0f + 0.7f) *
                              XMConvertToRadians(1.45f) * strength;
            const float pitch = std::sin(explosionShakeTime_ * 53.0f + 2.1f) *
                                XMConvertToRadians(1.05f) * strength;
            const XMVECTOR right = XMVector3Normalize(XMVector3Cross(front, up));
            front = XMVector3Normalize(front + right * std::tan(yaw) +
                                       up * std::tan(pitch));
            pos += right * (std::sin(explosionShakeTime_ * 43.0f) * 0.055f * strength);
            pos += up * (std::sin(explosionShakeTime_ * 61.0f + 1.3f) * 0.035f * strength);
        }
        return XMMatrixLookAtLH(pos, XMVectorAdd(pos, front), up);
    }

    void Update(float deltaTime) {
        explosionShakeTime_ += deltaTime;
        explosionTrauma_ = (std::max)(0.0f, explosionTrauma_ - deltaTime * 1.8f);
        explosionFovKick_ *= std::exp(-8.5f * (std::max)(0.0f, deltaTime));
        if (explosionFovKick_ < 0.01f) explosionFovKick_ = 0.0f;
        if (FPSMode) {
            if (IsSliding) {
                Position.x += SlideDirection.x * SlideSpeed * deltaTime;
                Position.z += SlideDirection.z * SlideSpeed * deltaTime;
                SlideSpeed = (std::max)(0.0f, SlideSpeed - 9.0f * deltaTime);
                SlideTimeRemaining -= deltaTime;
                if (SlideTimeRemaining <= 0.0f ||
                    SlideSpeed <= MovementSpeed || !IsGrounded) {
                    IsSliding = false;
                    SlideSpeed = 0.0f;
                }
            }

            const float groundLevel = FloorY + PlayerHeight;

            // Swimming starts once the feet are under the surface and the
            // seabed is too deep to stand on. Wading in the shallows keeps
            // normal walking, which is what stops the player from breaking into
            // a swim while still ankle-deep on the beach.
            const float feetY = Position.y - PlayerHeight;
            const bool standingDepth =
                WaterSurfaceY - FloorY < PlayerHeight * kWadeDepthFraction;
            IsSwimming = feetY < WaterSurfaceY && !standingDepth;

            if (IsSwimming) {
                // Buoyancy toward a float line that keeps the eyes just above
                // the surface, so the default state is treading water with the
                // head out rather than bobbing under it.
                const float floatLine =
                    WaterSurfaceY + PlayerHeight - kEyesUnderSurface;
                const float toSurface = floatLine - Position.y;
                // Two-sided spring, so treading water settles on the float line
                // instead of coasting to a stop wherever drag happens to win.
                // Submerged, buoyancy is full strength; above the line it only
                // has to cancel the overshoot, and it yields entirely to a dive
                // so holding crouch actually takes the player under.
                float buoyancy = toSurface * kBuoyancyStiffness;
                if (toSurface < 0.0f) buoyancy *= kAboveSurfaceBuoyancyScale;
                if (SwimInput < 0.0f) buoyancy = (std::min)(buoyancy, 0.0f);
                VerticalVelocity += (buoyancy + SwimInput * kSwimVerticalAccel) *
                                    deltaTime;
                // Water drag. Heavy enough that vertical motion settles instead
                // of oscillating around the float line.
                VerticalVelocity *= std::exp(-kWaterDrag * deltaTime);
                VerticalVelocity = (std::max)(-kMaxSwimSpeed,
                    (std::min)(kMaxSwimSpeed, VerticalVelocity));
                Position.y += VerticalVelocity * deltaTime;

                // The seabed still stops the player, so diving to the bottom
                // stands on it rather than sinking through.
                if (Position.y <= groundLevel) {
                    Position.y = groundLevel;
                    if (VerticalVelocity < 0.0f) VerticalVelocity = 0.0f;
                }
                // Never grounded while swimming: jump, slide and step-up all
                // gate on IsGrounded and none of them should fire mid-water.
                IsGrounded = false;
            } else {
                // Apply gravity
                VerticalVelocity -= Gravity * deltaTime;
                Position.y += VerticalVelocity * deltaTime;

                // Ground collision
                if (Position.y <= groundLevel) {
                    Position.y = groundLevel;
                    VerticalVelocity = 0.0f;
                    IsGrounded = true;
                } else {
                    IsGrounded = false;
                }
            }
            SwimInput = 0.0f;
        }
    }

    void ProcessKeyboard(char direction, float deltaTime, float speedMultiplier = 1.0f) {
        float velocity = MovementSpeed * speedMultiplier * deltaTime;
        
        if (FPSMode) {
            // In water the vertical keys swim instead of jumping. Space rises,
            // Q dives; Jump() is suppressed because there is no ground to push
            // off and it would fling the player out of the water.
            if (IsSwimming) {
                if (direction == ' ') { SwimInput += 1.0f; return; }
                if (direction == 'Q') { SwimInput -= 1.0f; return; }
                velocity *= kSwimSpeedScale;
            } else if (direction == ' ') {
                Jump();
                return;
            }
            // FPS walking mode - movement constrained to XZ plane
            XMFLOAT3 frontXZ = XMFLOAT3(Front.x, 0.0f, Front.z);
            XMVECTOR frontXZVec = XMVector3Normalize(XMLoadFloat3(&frontXZ));
            XMStoreFloat3(&frontXZ, frontXZVec);
            
            XMVECTOR upVec = XMLoadFloat3(&Up);
            XMVECTOR rightXZVec = XMVector3Normalize(XMVector3Cross(frontXZVec, upVec));
            XMFLOAT3 rightXZ;
            XMStoreFloat3(&rightXZ, rightXZVec);
            
            if (direction == 'W') {
                Position.x += frontXZ.x * velocity;
                Position.z += frontXZ.z * velocity;
            }
            if (direction == 'S') {
                Position.x -= frontXZ.x * velocity;
                Position.z -= frontXZ.z * velocity;
            }
            if (direction == 'D') {
                Position.x -= rightXZ.x * velocity;
                Position.z -= rightXZ.z * velocity;
            }
            if (direction == 'A') {
                Position.x += rightXZ.x * velocity;
                Position.z += rightXZ.z * velocity;
            }
        } else {
            // Free fly mode
            XMVECTOR pos = XMLoadFloat3(&Position);
            XMVECTOR front = XMLoadFloat3(&Front);
            XMVECTOR up = XMLoadFloat3(&Up);
            XMVECTOR right = XMVector3Normalize(XMVector3Cross(front, up));
            
            if (direction == 'W') pos = XMVectorAdd(pos, XMVectorScale(front, velocity));
            if (direction == 'S') pos = XMVectorSubtract(pos, XMVectorScale(front, velocity));
            if (direction == 'D') pos = XMVectorSubtract(pos, XMVectorScale(right, velocity));
            if (direction == 'A') pos = XMVectorAdd(pos, XMVectorScale(right, velocity));
            
            XMStoreFloat3(&Position, pos);
        }
    }

    void ProcessMouseMovement(float xoffset, float yoffset) {
        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;
        Yaw -= xoffset;
        Pitch += yoffset;
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;
        updateCameraVectors();
    }

    void SetCrouching(bool crouching, float deltaTime) {
        if (!FPSMode) return;
        constexpr float standingHeight = 1.7f;
        constexpr float crouchingHeight = 0.95f;
        constexpr float transitionSpeed = 5.0f;
        IsCrouching = crouching;
        const float target = crouching ? crouchingHeight : standingHeight;
        const float oldHeight = PlayerHeight;
        const float step = transitionSpeed * deltaTime;
        if (PlayerHeight < target)
            PlayerHeight = (std::min)(target, PlayerHeight + step);
        else
            PlayerHeight = (std::max)(target, PlayerHeight - step);
        if (IsGrounded) Position.y += PlayerHeight - oldHeight;
    }

    bool StartSlide(float forwardInput, float strafeInput) {
        if (!FPSMode || !IsGrounded || IsSliding) return false;

        XMVECTOR forward = XMVectorSet(Front.x, 0.0f, Front.z, 0.0f);
        if (XMVectorGetX(XMVector3LengthSq(forward)) <= 1e-6f) return false;
        forward = XMVector3Normalize(forward);
        const XMVECTOR right = XMVector3Normalize(XMVector3Cross(
            forward, XMLoadFloat3(&Up)));
        XMVECTOR direction = XMVectorAdd(
            XMVectorScale(forward, forwardInput),
            XMVectorScale(right, strafeInput));
        if (XMVectorGetX(XMVector3LengthSq(direction)) <= 1e-6f) return false;

        direction = XMVector3Normalize(direction);
        XMStoreFloat3(&SlideDirection, direction);
        SlideSpeed = MovementSpeed * 2.2f;
        SlideTimeRemaining = 0.65f;
        IsSliding = true;
        return true;
    }

    // Instant angular kick from weapon recoil. Unlike mouse input this is
    // already expressed in degrees, so sensitivity must not scale it.
    void ApplyRecoil(float pitchDegrees, float yawDegrees) {
        Pitch = (std::max)(-89.0f, (std::min)(89.0f, Pitch + pitchDegrees));
        Yaw += yawDegrees;
        updateCameraVectors();
    }

    void ApplyExplosionImpulse(const XMFLOAT3& source, float visualSize) {
        const float dx = source.x - Position.x;
        const float dy = source.y - Position.y;
        const float dz = source.z - Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float reach = (std::max)(12.0f, visualSize * 5.5f);
        const float falloff = (std::max)(0.0f, 1.0f - distance / reach);
        if (falloff <= 0.0f) return;
        explosionTrauma_ = (std::min)(1.0f, explosionTrauma_ +
            falloff * (0.45f + visualSize * 0.055f));
        explosionFovKick_ = (std::max)(explosionFovKick_, 3.2f * falloff);
    }

    // Sharp jolt from taking a hit. Feeds the same shake the explosion uses so
    // there is one place that moves the view, but capped well below a blast:
    // being shot should punch the aim off, not blind the player.
    void AddHitTrauma(float amount) {
        explosionTrauma_ = (std::min)(0.72f, explosionTrauma_ + amount);
    }

    // Small kick from firing. Same shake channel again, with a much lower cap
    // than either a blast or a hit: sustained automatic fire adds a tap per
    // round, and without its own ceiling a held trigger would ramp the view up
    // to grenade-level shake within a second.
    //
    // Deliberately capped BELOW where the shake gets disorienting rather than
    // scaled per shot, because the trauma curve is squared -- at 0.22 the shake
    // is a tremor, and letting it stack to 0.5 would be four times that.
    //
    // The cap only limits what firing itself may ADD. Trauma already above it
    // (a grenade just went off, the player was just shot) is left alone rather
    // than clamped down -- otherwise pulling the trigger during a blast would
    // cancel the blast's shake, which is backwards.
    void AddFireTrauma(float amount) {
        constexpr float kFireTraumaCeiling = 0.22f;
        if (explosionTrauma_ >= kFireTraumaCeiling) return;
        explosionTrauma_ =
            (std::min)(kFireTraumaCeiling, explosionTrauma_ + amount);
    }

    float ExplosionFovKick() const { return explosionFovKick_; }

    void Jump() {
        if (FPSMode && IsGrounded && !IsCrouching) {
            VerticalVelocity = JumpStrength;
            IsGrounded = false;
        }
    }

private:
    float explosionTrauma_ = 0.0f;
    float explosionShakeTime_ = 0.0f;
    float explosionFovKick_ = 0.0f;

    void updateCameraVectors() {
        float yawRad = XMConvertToRadians(Yaw);
        float pitchRad = XMConvertToRadians(Pitch);
        
        XMFLOAT3 front;
        front.x = cosf(yawRad) * cosf(pitchRad);
        front.y = sinf(pitchRad);
        front.z = sinf(yawRad) * cosf(pitchRad);
        
        XMVECTOR frontVec = XMVector3Normalize(XMLoadFloat3(&front));
        XMStoreFloat3(&Front, frontVec);
    }
};

#endif

