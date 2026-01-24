#ifndef CAMERA_DX11_H
#define CAMERA_DX11_H

#include <DirectXMath.h>
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
    
    // Jump mechanics
    bool IsGrounded;
    float VerticalVelocity;
    float Gravity;
    float JumpStrength;

    Camera(XMFLOAT3 position = XMFLOAT3(0.0f, 5.0f, 10.0f))
        : Position(position), Front(XMFLOAT3(0.0f, 0.0f, -1.0f)), Up(XMFLOAT3(0.0f, 1.0f, 0.0f)),
          Yaw(-90.0f), Pitch(-20.0f), MovementSpeed(2.5f), MouseSensitivity(0.1f),
          FPSMode(false), PlayerHeight(1.7f), FloorY(-0.5f),
          IsGrounded(true), VerticalVelocity(0.0f), Gravity(9.8f), JumpStrength(5.0f) {
        updateCameraVectors();
    }

    XMMATRIX GetViewMatrix() {
        XMVECTOR pos = XMLoadFloat3(&Position);
        XMVECTOR front = XMLoadFloat3(&Front);
        XMVECTOR up = XMLoadFloat3(&Up);
        return XMMatrixLookAtLH(pos, XMVectorAdd(pos, front), up);
    }

    void Update(float deltaTime) {
        if (FPSMode) {
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

    void ProcessKeyboard(char direction, float deltaTime) {
        float velocity = MovementSpeed * deltaTime;
        
        if (FPSMode) {
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

    void Jump() {
        if (FPSMode && IsGrounded) {
            VerticalVelocity = JumpStrength;
            IsGrounded = false;
        }
    }

private:
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

