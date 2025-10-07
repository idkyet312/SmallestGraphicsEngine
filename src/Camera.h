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

    Camera(glm::vec3 position = glm::vec3(0.0f, 5.0f, 10.0f))
        : Position(position), Front(glm::vec3(0.0f, 0.0f, -1.0f)), Up(glm::vec3(0.0f, 1.0f, 0.0f)),
          Yaw(-90.0f), Pitch(-20.0f) {
        updateCameraVectors();
    }

    glm::mat4 GetViewMatrix() {
        return glm::lookAt(Position, Position + Front, Up);
    }

    void ProcessKeyboard(char direction, float deltaTime) {
        float velocity = 2.5f * deltaTime;
        if (direction == 'W') Position += Front * velocity;
        if (direction == 'S') Position -= Front * velocity;
        if (direction == 'A') Position -= glm::normalize(glm::cross(Front, Up)) * velocity;
        if (direction == 'D') Position += glm::normalize(glm::cross(Front, Up)) * velocity;
    }

    void ProcessMouseMovement(float xoffset, float yoffset) {
        xoffset *= 0.1f;
        yoffset *= 0.1f;
        Yaw += xoffset;
        Pitch += yoffset;
        if (Pitch > 89.0f) Pitch = 89.0f;
        if (Pitch < -89.0f) Pitch = -89.0f;
        updateCameraVectors();
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
