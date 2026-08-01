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

            // Apply gravity
            float groundLevel = FloorY + PlayerHeight;
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
    }

    void ProcessKeyboard(char direction, float deltaTime, float speedMultiplier = 1.0f) {
        float velocity = MovementSpeed * speedMultiplier * deltaTime;
        
        if (FPSMode) {
            if (direction == ' ') { Jump(); return; }
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

