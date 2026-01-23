#ifndef CAMERA_H
#define CAMERA_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    glm::vec3 Position;
    glm::vec3 Front;
    glm::vec3 Up;
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

    Camera(glm::vec3 position = glm::vec3(0.0f, 5.0f, 10.0f))
        : Position(position), Front(glm::vec3(0.0f, 0.0f, -1.0f)), Up(glm::vec3(0.0f, 1.0f, 0.0f)),
          Yaw(-90.0f), Pitch(-20.0f), MovementSpeed(2.5f), MouseSensitivity(0.1f),
          FPSMode(false), PlayerHeight(1.7f), FloorY(-0.5f),
          IsGrounded(true), VerticalVelocity(0.0f), Gravity(9.8f), JumpStrength(5.0f) {
        updateCameraVectors();
    }

    glm::mat4 GetViewMatrix() {
        return glm::lookAt(Position, Position + Front, Up);
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
            glm::vec3 frontXZ = glm::normalize(glm::vec3(Front.x, 0.0f, Front.z));
            glm::vec3 rightXZ = glm::normalize(glm::cross(frontXZ, Up));
            
            if (direction == 'W') {
                Position.x += frontXZ.x * velocity;
                Position.z += frontXZ.z * velocity;
            }
            if (direction == 'S') {
                Position.x -= frontXZ.x * velocity;
                Position.z -= frontXZ.z * velocity;
            }
            if (direction == 'A') {
                Position.x -= rightXZ.x * velocity;
                Position.z -= rightXZ.z * velocity;
            }
            if (direction == 'D') {
                Position.x += rightXZ.x * velocity;
                Position.z += rightXZ.z * velocity;
            }
        } else {
            // Free fly mode - original behavior
            if (direction == 'W') Position += Front * velocity;
            if (direction == 'S') Position -= Front * velocity;
            if (direction == 'A') Position -= glm::normalize(glm::cross(Front, Up)) * velocity;
            if (direction == 'D') Position += glm::normalize(glm::cross(Front, Up)) * velocity;
        }
    }

    void ProcessMouseMovement(float xoffset, float yoffset) {
        xoffset *= MouseSensitivity;
        yoffset *= MouseSensitivity;
        Yaw += xoffset;
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
        glm::vec3 front;
        front.x = cos(glm::radians(Yaw)) * cos(glm::radians(Pitch));
        front.y = sin(glm::radians(Pitch));
        front.z = sin(glm::radians(Yaw)) * cos(glm::radians(Pitch));
        Front = glm::normalize(front);
    }
};

#endif
