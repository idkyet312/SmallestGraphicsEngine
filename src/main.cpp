#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <vector>
#include <string>
#include "Shader.h"
#include "Camera.h"
#include "Model.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Settings
unsigned int SCR_WIDTH = 1280;
unsigned int SCR_HEIGHT = 720;
unsigned int SHADOW_WIDTH = 2048;
unsigned int SHADOW_HEIGHT = 2048;

Camera camera(glm::vec3(0.0f, 5.0f, 10.0f));
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
float deltaTime = 0.0f;
float lastFrame = 0.0f;
bool showUI = true;
bool cameraLocked = false;

// Editable scene parameters
glm::vec3 lightPos(-5.0f, 10.0f, -5.0f);
glm::vec3 lightTarget(0.0f, 0.0f, 0.0f);  // Where the light looks at
glm::vec3 lightUp(0.0f, 1.0f, 0.0f);      // Light's up vector
glm::vec3 cubePosition(0.0f, 1.5f, 0.0f);
glm::vec3 cubeScale(1.0f, 1.0f, 1.0f);
glm::vec3 cubeRotation(0.0f, 0.0f, 0.0f); // Rotation in degrees (X, Y, Z)
glm::vec3 cubeColor(0.8f, 0.2f, 0.2f);
glm::vec3 floorColor(0.5f, 0.5f, 0.5f);
glm::vec3 clearColor(0.1f, 0.1f, 0.1f);
float cameraFOV = 45.0f;
float cameraNear = 0.1f;
float cameraFar = 100.0f;
float lightOrthoSize = 15.0f;
float lightNear = 1.0f;
float lightFar = 25.0f;

// Projection type (0 = perspective, 1 = orthographic)
int projectionType = 0;
float orthoSize = 10.0f;

// Shadow and rendering settings
float shadowBias = 0.005f;
bool enableShadows = true;
bool wireframeMode = false;
float ambientStrength = 0.3f;
float specularStrength = 0.5f;
int specularShininess = 32;

// Debug visualization
int renderMode = 0; // 0 = normal, 1 = shadow map depth, 2 = camera depth
float depthNear = 0.1f;
float depthFar = 100.0f;
bool showShadowMapOverlay = false;
float overlaySize = 0.25f; // Size of overlay (0.0 to 1.0)

// Additional objects
bool showSecondCube = true;
glm::vec3 cube2Position(-3.0f, 0.5f, 2.0f);
glm::vec3 cube2Scale(0.5f, 0.5f, 0.5f);
glm::vec3 cube2Rotation(0.0f, 45.0f, 0.0f);
glm::vec3 cube2Color(0.2f, 0.8f, 0.2f);

// Animation
bool animateLight = false;
bool animateCube = false;
float animationSpeed = 1.0f;

// Light properties (add after existing light variables)
int lightType = 0; // 0 = directional, 1 = point
float lightConstant = 1.0f;
float lightLinear = 0.09f;
float lightQuadratic = 0.032f;
glm::vec3 lightColor(1.0f, 1.0f, 1.0f);

// Second light
bool enableSecondLight = true;
glm::vec3 light2Pos(5.0f, 8.0f, 5.0f);
glm::vec3 light2Color(0.5f, 0.5f, 1.0f); // Blueish light
float light2Intensity = 0.8f;

// Skybox settings
bool enableSkybox = true;
glm::vec3 skyboxTint(1.0f, 1.0f, 1.0f);
bool enableSkyboxLighting = true;
float skyboxLightIntensity = 0.3f;
int skyboxMode = 0; // 0 = procedural gradient, 1 = load cubemap files, 2 = load HDR
char skyboxPath[256] = "skybox/";
bool skyboxNeedsReload = false;

// Ambient Occlusion settings
bool enableAO = true;
float aoStrength = 0.5f;
float aoPower = 2.0f;

// PBR settings
bool usePBR = true;
float metallic = 0.0f;
float roughness = 0.5f;
float pbrAO = 1.0f;

// Model loading
Model loadedModel;
bool showModel = false;
char modelPath[256] = "models/";
glm::vec3 modelPosition(0.0f, 0.0f, 0.0f);
glm::vec3 modelRotation(0.0f, 0.0f, 0.0f);
glm::vec3 modelScale(1.0f, 1.0f, 1.0f);
glm::vec3 modelColor(1.0f, 1.0f, 1.0f);

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (cameraLocked || (showUI && ImGui::GetIO().WantCaptureMouse))
        return;
    
    if (firstMouse) {
        lastX = (float)xpos;
        lastY = (float)ypos;
        firstMouse = false;
        return; // Skip first frame to avoid jump
    }
    
    float xoffset = (float)xpos - lastX;
    float yoffset = lastY - (float)ypos; // reversed
    
    lastX = (float)xpos;
    lastY = (float)ypos;
    
    camera.ProcessMouseMovement(xoffset, yoffset);
}

void mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        if (!ImGui::GetIO().WantCaptureMouse) {
            // Lock cursor to window
            cameraLocked = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            // Reset cursor position to center
            glfwSetCursorPos(window, SCR_WIDTH / 2.0, SCR_HEIGHT / 2.0);
            lastX = SCR_WIDTH / 2.0f;
            lastY = SCR_HEIGHT / 2.0f;
            firstMouse = true;
        }
    }
}

void processInput(GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
    
    // Toggle UI with Tab key
    static bool tabPressed = false;
    if (glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS && !tabPressed) {
        showUI = !showUI;
        if (showUI) {
            // Show UI - release cursor
            cameraLocked = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        tabPressed = true;
    }
    if (glfwGetKey(window, GLFW_KEY_TAB) == GLFW_RELEASE) {
        tabPressed = false;
    }
    
    // Release cursor with C key
    static bool cPressed = false;
    if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS && !cPressed) {
        // Release cursor lock
        cameraLocked = true;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        cPressed = true;
    }
    if (glfwGetKey(window, GLFW_KEY_C) == GLFW_RELEASE) {
        cPressed = false;
    }
    
    if (!cameraLocked && (!showUI || !ImGui::GetIO().WantCaptureKeyboard)) {
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
            camera.ProcessKeyboard('W', deltaTime);
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
            camera.ProcessKeyboard('S', deltaTime);
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            camera.ProcessKeyboard('A', deltaTime);
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            camera.ProcessKeyboard('D', deltaTime);
    }
}

unsigned int loadCubeVAO() {
    float vertices[] = {
        // positions          // normals
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
         0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        -0.5f, -0.5f, -0.5f,  0.0f,  0.0f, -1.0f,
        -0.5f,  0.5f, -0.5f,  0.0f,  0.0f, -1.0f,

        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f, 1.0f,
         0.5f, -0.5f,  0.5f,  0.0f,  0.0f, 1.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f, 1.0f,
         0.5f,  0.5f,  0.5f,  0.0f,  0.0f, 1.0f,
        -0.5f,  0.5f,  0.5f,  0.0f,  0.0f, 1.0f,
        -0.5f, -0.5f,  0.5f,  0.0f,  0.0f, 1.0f,

        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f,  0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f, -0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f, -0.5f,  0.5f, -1.0f,  0.0f,  0.0f,
        -0.5f,  0.5f,  0.5f, -1.0f,  0.0f,  0.0f,

         0.5f,  0.5f,  0.5f, 1.0f,  0.0f,  0.0f,
         0.5f, -0.5f, -0.5f, 1.0f,  0.0f,  0.0f,
         0.5f,  0.5f, -0.5f, 1.0f,  0.0f,  0.0f,
         0.5f, -0.5f, -0.5f, 1.0f,  0.0f,  0.0f,
         0.5f,  0.5f,  0.5f, 1.0f,  0.0f,  0.0f,
         0.5f, -0.5f,  0.5f, 1.0f,  0.0f,  0.0f,

        -0.5f, -0.5f, -0.5f, 0.0f, -1.0f,  0.0f,
         0.5f, -0.5f, -0.5f, 0.0f, -1.0f,  0.0f,
         0.5f, -0.5f,  0.5f, 0.0f, -1.0f,  0.0f,
         0.5f, -0.5f,  0.5f, 0.0f, -1.0f,  0.0f,
        -0.5f, -0.5f,  0.5f, 0.0f, -1.0f,  0.0f,
        -0.5f, -0.5f, -0.5f, 0.0f, -1.0f,  0.0f,

        -0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  0.0f,
         0.5f,  0.5f,  0.5f, 0.0f, 1.0f,  0.0f,
         0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  0.0f,
         0.5f,  0.5f,  0.5f, 0.0f, 1.0f,  0.0f,
        -0.5f,  0.5f, -0.5f, 0.0f, 1.0f,  0.0f,
        -0.5f,  0.5f,  0.5f, 0.0f, 1.0f,  0.0f
    };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
    return VAO;
}

unsigned int loadPlaneVAO() {
    float planeVertices[] = {
        // positions            // normals
         25.0f, -0.5f,  25.0f,  0.0f, 1.0f, 0.0f,
        -25.0f, -0.5f, -25.0f,  0.0f, 1.0f, 0.0f,
        -25.0f, -0.5f,  25.0f,  0.0f, 1.0f, 0.0f,

         25.0f, -0.5f,  25.0f,  0.0f, 1.0f, 0.0f,
         25.0f, -0.5f, -25.0f,  0.0f, 1.0f, 0.0f,
        -25.0f, -0.5f, -25.0f,  0.0f, 1.0f, 0.0f
    };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(planeVertices), planeVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
    return VAO;
}

unsigned int loadQuadVAO() {
    float quadVertices[] = {
        // positions        // texture Coords
        -1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
         1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
    };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), &quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
    return VAO;
}

unsigned int loadSkyboxVAO() {
    float skyboxVertices[] = {
        // positions          
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f
    };
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
    return VAO;
}

unsigned int createGradientSkyboxTexture() {
    unsigned int textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);

    const int size = 512;
    unsigned char* data = new unsigned char[size * size * 3];

    // Colors for gradient (top to bottom)
    glm::vec3 skyTop(0.3f, 0.5f, 0.9f);      // Darker blue at top
    glm::vec3 skyHorizon(0.7f, 0.8f, 1.0f);  // Lighter blue at horizon
    glm::vec3 skyBottom(0.4f, 0.4f, 0.45f);  // Darker at bottom

    // Create each face with proper gradient based on direction
    for (int face = 0; face < 6; face++) {
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                // Convert pixel to normalized cubemap coordinates [-1, 1]
                float u = (float)x / (float)(size - 1) * 2.0f - 1.0f;
                float v = (float)y / (float)(size - 1) * 2.0f - 1.0f;
                
                // Get direction vector for this face
                glm::vec3 dir;
                switch (face) {
                    case 0: dir = glm::normalize(glm::vec3( 1.0f, -v, -u)); break; // +X
                    case 1: dir = glm::normalize(glm::vec3(-1.0f, -v,  u)); break; // -X
                    case 2: dir = glm::normalize(glm::vec3( u,  1.0f,  v)); break; // +Y
                    case 3: dir = glm::normalize(glm::vec3( u, -1.0f, -v)); break; // -Y
                    case 4: dir = glm::normalize(glm::vec3( u, -v,  1.0f)); break; // +Z
                    case 5: dir = glm::normalize(glm::vec3(-u, -v, -1.0f)); break; // -Z
                }
                
                // Use Y component to determine gradient
                float height = dir.y; // -1 to 1
                glm::vec3 color;
                if (height > 0.0f) {
                    // Upper hemisphere - blend from horizon to sky top
                    color = glm::mix(skyHorizon, skyTop, height);
                } else {
                    // Lower hemisphere - blend from horizon to bottom
                    color = glm::mix(skyHorizon, skyBottom, -height);
                }
                
                int index = (y * size + x) * 3;
                data[index + 0] = (unsigned char)(glm::clamp(color.r, 0.0f, 1.0f) * 255);
                data[index + 1] = (unsigned char)(glm::clamp(color.g, 0.0f, 1.0f) * 255);
                data[index + 2] = (unsigned char)(glm::clamp(color.b, 0.0f, 1.0f) * 255);
            }
        }
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB, size, size, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    }

    delete[] data;

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    return textureID;
}

unsigned int loadCubemapFromFiles(std::vector<std::string> faces) {
    unsigned int textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);

    int width, height, nrChannels;
    for (unsigned int i = 0; i < faces.size(); i++) {
        unsigned char *data = stbi_load(faces[i].c_str(), &width, &height, &nrChannels, 0);
        if (data) {
            GLenum format = GL_RGB;
            if (nrChannels == 1)
                format = GL_RED;
            else if (nrChannels == 3)
                format = GL_RGB;
            else if (nrChannels == 4)
                format = GL_RGBA;

            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
            std::cout << "Loaded cubemap face: " << faces[i] << std::endl;
        } else {
            std::cout << "Cubemap texture failed to load at path: " << faces[i] << std::endl;
            stbi_image_free(data);
            // Return generated gradient if loading fails
            return createGradientSkyboxTexture();
        }
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    return textureID;
}

unsigned int loadHDREnvironment(const char* filepath) {
    stbi_set_flip_vertically_on_load(true);
    int width, height, nrComponents;
    float *data = stbi_loadf(filepath, &width, &height, &nrComponents, 0);
    
    if (!data) {
        std::cout << "Failed to load HDR image: " << filepath << std::endl;
        return createGradientSkyboxTexture();
    }

    std::cout << "HDR loaded: " << width << "x" << height << " with " << nrComponents << " components" << std::endl;
    std::cout << "Converting equirectangular to cubemap..." << std::endl;

    // Create cubemap
    unsigned int cubemap;
    glGenTextures(1, &cubemap);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap);
    
    // Allocate cubemap faces (1024x1024 for each face)
    int cubemapSize = 1024;
    for (unsigned int i = 0; i < 6; i++) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB16F,
                     cubemapSize, cubemapSize, 0, GL_RGB, GL_FLOAT, nullptr);
    }
    
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // CPU-based equirectangular to cubemap conversion
    std::vector<glm::vec3> faceData(cubemapSize * cubemapSize);
    const float PI = 3.14159265359f;
    
    for (int face = 0; face < 6; face++) {
        for (int y = 0; y < cubemapSize; y++) {
            for (int x = 0; x < cubemapSize; x++) {
                // Calculate direction for this cubemap texel
                float u = (x + 0.5f) / cubemapSize * 2.0f - 1.0f;
                float v = (y + 0.5f) / cubemapSize * 2.0f - 1.0f;
                
                glm::vec3 dir;
                switch (face) {
                    case 0: dir = glm::normalize(glm::vec3( 1.0f, -v, -u)); break; // +X (right)
                    case 1: dir = glm::normalize(glm::vec3(-1.0f, -v,  u)); break; // -X (left)
                    case 2: dir = glm::normalize(glm::vec3( u,  1.0f,  v)); break; // +Y (top)
                    case 3: dir = glm::normalize(glm::vec3( u, -1.0f, -v)); break; // -Y (bottom)
                    case 4: dir = glm::normalize(glm::vec3( u, -v,  1.0f)); break; // +Z (front)
                    case 5: dir = glm::normalize(glm::vec3(-u, -v, -1.0f)); break; // -Z (back)
                }
                
                // Convert direction to equirectangular coordinates (longitude/latitude)
                float phi = atan2(dir.z, dir.x);
                float theta = asin(glm::clamp(dir.y, -1.0f, 1.0f));
                
                // Map to texture coordinates
                float equiU = (phi / (2.0f * PI)) + 0.5f;
                float equiV = (theta / PI) + 0.5f;
                
                // Sample from HDR data (bilinear interpolation)
                equiU = glm::clamp(equiU, 0.0f, 1.0f);
                equiV = glm::clamp(equiV, 0.0f, 1.0f);
                
                float px = equiU * (width - 1);
                float py = equiV * (height - 1);
                
                int x0 = (int)floor(px);
                int x1 = (int)ceil(px);
                int y0 = (int)floor(py);
                int y1 = (int)ceil(py);
                
                float fx = px - x0;
                float fy = py - y0;
                
                // Clamp indices
                x0 = glm::clamp(x0, 0, width - 1);
                x1 = glm::clamp(x1, 0, width - 1);
                y0 = glm::clamp(y0, 0, height - 1);
                y1 = glm::clamp(y1, 0, height - 1);
                
                // Get pixel indices
                int idx00 = (y0 * width + x0) * nrComponents;
                int idx10 = (y0 * width + x1) * nrComponents;
                int idx01 = (y1 * width + x0) * nrComponents;
                int idx11 = (y1 * width + x1) * nrComponents;
                
                // Bilinear interpolation
                glm::vec3 color00(data[idx00], data[idx00 + 1], data[idx00 + 2]);
                glm::vec3 color10(data[idx10], data[idx10 + 1], data[idx10 + 2]);
                glm::vec3 color01(data[idx01], data[idx01 + 1], data[idx01 + 2]);
                glm::vec3 color11(data[idx11], data[idx11 + 1], data[idx11 + 2]);
                
                glm::vec3 color0 = glm::mix(color00, color10, fx);
                glm::vec3 color1 = glm::mix(color01, color11, fx);
                glm::vec3 finalColor = glm::mix(color0, color1, fy);
                
                faceData[y * cubemapSize + x] = finalColor;
            }
        }
        
        glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0,
                       cubemapSize, cubemapSize, GL_RGB, GL_FLOAT, faceData.data());
        
        std::cout << "  Face " << face << "/6 converted" << std::endl;
    }
    
    stbi_image_free(data);
    
    std::cout << "HDR converted to cubemap successfully!" << std::endl;
    return cubemap;
}

int main() {
    // Initialize GLFW
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "Shadow Mapping Engine", NULL, NULL);
    if (window == NULL) {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    
    // Start with camera locked (cursor visible)
    cameraLocked = true;
    
    // Disable VSync to uncap frame rate
    glfwSwapInterval(0);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    glEnable(GL_DEPTH_TEST);

    Shader depthShader("shaders/depth.vert", "shaders/depth.frag");
    Shader shadowShader("shaders/shadow.vert", "shaders/shadow.frag");
    Shader pbrShader("shaders/pbr.vert", "shaders/pbr.frag");
    Shader debugDepthShader("shaders/debug_depth.vert", "shaders/debug_depth.frag");
    Shader skyboxShader("shaders/skybox.vert", "shaders/skybox.frag");

    unsigned int depthMapFBO;
    glGenFramebuffers(1, &depthMapFBO);

    unsigned int depthMap;
    glGenTextures(1, &depthMap);
    glBindTexture(GL_TEXTURE_2D, depthMap);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthMap, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Second shadow map for light 2
    unsigned int depthMapFBO2;
    glGenFramebuffers(1, &depthMapFBO2);

    unsigned int depthMap2;
    glGenTextures(1, &depthMap2);
    glBindTexture(GL_TEXTURE_2D, depthMap2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

    glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO2);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthMap2, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    unsigned int cubeVAO = loadCubeVAO();
    unsigned int planeVAO = loadPlaneVAO();
    unsigned int quadVAO = loadQuadVAO();
    unsigned int skyboxVAO = loadSkyboxVAO();
    unsigned int skyboxTexture = createGradientSkyboxTexture();

    pbrShader.use();
    pbrShader.setInt("shadowMap", 0);
    pbrShader.setInt("shadowMap2", 1);
    pbrShader.setInt("skybox", 2);
    
    // Set default light attenuation values
    pbrShader.setFloat("constant", lightConstant);
    pbrShader.setFloat("linear", lightLinear);
    pbrShader.setFloat("quadratic", lightQuadratic);
    
    debugDepthShader.use();
    debugDepthShader.setInt("depthMap", 0);

    skyboxShader.use();
    skyboxShader.setInt("skybox", 0);

    while (!glfwWindowShouldClose(window)) {
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        processInput(window);

        // Reload skybox if needed
        if (skyboxNeedsReload) {
            glDeleteTextures(1, &skyboxTexture);
            
            if (skyboxMode == 0) {
                // Procedural gradient
                skyboxTexture = createGradientSkyboxTexture();
            } else if (skyboxMode == 1) {
                // Load cubemap from files
                std::string path = std::string(skyboxPath);
                if (path.back() != '/' && path.back() != '\\') path += "/";
                
                std::vector<std::string> faces = {
                    path + "right.jpg",
                    path + "left.jpg",
                    path + "top.jpg",
                    path + "bottom.jpg",
                    path + "front.jpg",
                    path + "back.jpg"
                };
                skyboxTexture = loadCubemapFromFiles(faces);
            } else if (skyboxMode == 2) {
                // Load HDR
                skyboxTexture = loadHDREnvironment(skyboxPath);
            }
            
            skyboxNeedsReload = false;
        }

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create ImGui UI
        if (showUI) {
            ImGui::Begin("Scene Controls", &showUI);
            
            ImGui::Text("Controls:");
            ImGui::BulletText("TAB: Toggle UI");
            ImGui::BulletText("C: Lock/Unlock Camera");
            
            // Camera lock status button
            if (cameraLocked) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("Camera LOCKED (Press C to unlock)")) {
                    cameraLocked = false;
                }
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
                if (ImGui::Button("Camera UNLOCKED (Press C to lock)")) {
                    cameraLocked = true;
                }
                ImGui::PopStyleColor();
            }
            
            ImGui::Separator();
            
            if (ImGui::CollapsingHeader("Camera Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3("Camera Position", &camera.Position.x, 0.1f);
                ImGui::DragFloat("FOV", &cameraFOV, 0.5f, 1.0f, 120.0f);
                ImGui::DragFloat("Near Plane", &cameraNear, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("Far Plane", &cameraFar, 1.0f, 10.0f, 500.0f);
                ImGui::DragFloat("Movement Speed", &camera.MovementSpeed, 0.1f, 0.1f, 50.0f);
                ImGui::DragFloat("Mouse Sensitivity", &camera.MouseSensitivity, 0.001f, 0.001f, 1.0f);
                ImGui::Separator();
                ImGui::Text("Camera Projection");
                const char* projTypes[] = { "Perspective", "Orthographic" };
                ImGui::Combo("Projection Type", &projectionType, projTypes, 2);
                if (projectionType == 1) {
                    ImGui::DragFloat("Ortho Size", &orthoSize, 0.5f, 0.1f, 50.0f);
                }
            }
            
            if (ImGui::CollapsingHeader("Light Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Light Type");
                const char* lightTypes[] = { "Directional", "Point Light" };
                ImGui::Combo("Light Type", &lightType, lightTypes, 2);
                
                ImGui::Separator();
                ImGui::Text("Light Transform");
                ImGui::DragFloat3("Light Position", &lightPos.x, 0.1f);
                ImGui::ColorEdit3("Light Color", &lightColor.x);
                
                if (lightType == 0) {
                    // Directional light settings
                    ImGui::DragFloat3("Light Target", &lightTarget.x, 0.1f);
                    ImGui::DragFloat3("Light Up Vector", &lightUp.x, 0.01f);
                } else {
                    // Point light attenuation
                    ImGui::Separator();
                    ImGui::Text("Point Light Attenuation");
                    ImGui::DragFloat("Constant", &lightConstant, 0.01f, 0.0f, 10.0f);
                    ImGui::DragFloat("Linear", &lightLinear, 0.001f, 0.0f, 1.0f, "%.4f");
                    ImGui::DragFloat("Quadratic", &lightQuadratic, 0.001f, 0.0f, 1.0f, "%.4f");
                    
                    // Presets
                    if (ImGui::Button("Distance 7")) {
                        lightConstant = 1.0f; lightLinear = 0.7f; lightQuadratic = 1.8f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Distance 13")) {
                        lightConstant = 1.0f; lightLinear = 0.35f; lightQuadratic = 0.44f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Distance 20")) {
                        lightConstant = 1.0f; lightLinear = 0.22f; lightQuadratic = 0.20f;
                    }
                    if (ImGui::Button("Distance 32")) {
                        lightConstant = 1.0f; lightLinear = 0.14f; lightQuadratic = 0.07f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Distance 50")) {
                        lightConstant = 1.0f; lightLinear = 0.09f; lightQuadratic = 0.032f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Distance 100")) {
                        lightConstant = 1.0f; lightLinear = 0.045f; lightQuadratic = 0.0075f;
                    }
                    if (ImGui::Button("Distance 200")) {
                        lightConstant = 1.0f; lightLinear = 0.022f; lightQuadratic = 0.0019f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Distance 325")) {
                        lightConstant = 1.0f; lightLinear = 0.014f; lightQuadratic = 0.0007f;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Distance 600")) {
                        lightConstant = 1.0f; lightLinear = 0.007f; lightQuadratic = 0.0002f;
                    }
                }
                
                ImGui::Separator();
                ImGui::Text("Light Projection (Ortho)");
                ImGui::DragFloat("Light Ortho Size", &lightOrthoSize, 0.5f, 1.0f, 50.0f);
                ImGui::DragFloat("Light Near", &lightNear, 0.1f, 0.1f, 20.0f);
                ImGui::DragFloat("Light Far", &lightFar, 0.5f, 1.0f, 100.0f);
                ImGui::Separator();
                ImGui::Text("Animation");
                ImGui::Checkbox("Animate Light", &animateLight);
                if (animateLight) {
                    ImGui::SameLine();
                    ImGui::DragFloat("Speed##light", &animationSpeed, 0.1f, 0.1f, 10.0f);
                }
                
                // Second light controls
                ImGui::Separator();
                ImGui::Text("Second Light");
                ImGui::Checkbox("Enable Second Light", &enableSecondLight);
                if (enableSecondLight) {
                    ImGui::DragFloat3("Light 2 Position", &light2Pos.x, 0.1f);
                    ImGui::ColorEdit3("Light 2 Color", &light2Color.x);
                    ImGui::SliderFloat("Light 2 Intensity", &light2Intensity, 0.0f, 2.0f);
                }
            }
            
            if (ImGui::CollapsingHeader("Cube 1 Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3("Position##cube1", &cubePosition.x, 0.1f);
                ImGui::DragFloat3("Rotation (deg)##cube1", &cubeRotation.x, 1.0f, -360.0f, 360.0f);
                ImGui::DragFloat3("Scale##cube1", &cubeScale.x, 0.1f, 0.1f, 10.0f);
                ImGui::ColorEdit3("Color##cube1", &cubeColor.x);
                ImGui::Checkbox("Animate##cube1", &animateCube);
            }
            
            if (ImGui::CollapsingHeader("Cube 2 Settings")) {
                ImGui::Checkbox("Show Second Cube", &showSecondCube);
                if (showSecondCube) {
                    ImGui::DragFloat3("Position##cube2", &cube2Position.x, 0.1f);
                    ImGui::DragFloat3("Rotation (deg)##cube2", &cube2Rotation.x, 1.0f, -360.0f, 360.0f);
                    ImGui::DragFloat3("Scale##cube2", &cube2Scale.x, 0.1f, 0.1f, 10.0f);
                    ImGui::ColorEdit3("Color##cube2", &cube2Color.x);
                }
            }
            
            if (ImGui::CollapsingHeader("Rendering Settings")) {
                ImGui::ColorEdit3("Floor Color", &floorColor.x);
                ImGui::ColorEdit3("Clear Color", &clearColor.x);
                ImGui::Separator();
                ImGui::Text("Lighting");
                ImGui::DragFloat("Ambient Strength", &ambientStrength, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Specular Strength", &specularStrength, 0.01f, 0.0f, 1.0f);
                ImGui::SliderInt("Specular Shininess", &specularShininess, 1, 256);
                ImGui::Separator();
                ImGui::Text("Shadows");
                ImGui::Checkbox("Enable Shadows", &enableShadows);
                ImGui::DragFloat("Shadow Bias", &shadowBias, 0.0001f, 0.0f, 0.1f, "%.4f");
                ImGui::Separator();
                ImGui::Checkbox("Wireframe Mode", &wireframeMode);
                ImGui::Separator();
                ImGui::Text("Debug Visualization");
                const char* renderModes[] = { "Normal", "Light Depth Map", "Camera Depth" };
                ImGui::Combo("Render Mode", &renderMode, renderModes, 3);
                if (renderMode > 0) {
                    ImGui::Text("Depth range visualization");
                }
                ImGui::Separator();
                ImGui::Text("Shadow Map Debug");
                ImGui::Checkbox("Show Shadow Map Overlay", &showShadowMapOverlay);
                if (showShadowMapOverlay) {
                    ImGui::SliderFloat("Overlay Size", &overlaySize, 0.1f, 0.5f);
                }
                ImGui::Separator();
                ImGui::Text("Skybox");
                ImGui::Checkbox("Enable Skybox", &enableSkybox);
                if (enableSkybox) {
                    ImGui::ColorEdit3("Skybox Tint", &skyboxTint.x);
                    ImGui::Separator();
                    
                    ImGui::Text("Skybox Source");
                    const char* skyboxModes[] = { "Procedural Gradient", "Load Cubemap", "Load HDR" };
                    if (ImGui::Combo("Skybox Mode", &skyboxMode, skyboxModes, 3)) {
                        skyboxNeedsReload = true;
                    }
                    
                    if (skyboxMode == 1) {
                        ImGui::Text("Place 6 images named:");
                        ImGui::BulletText("right.jpg, left.jpg, top.jpg");
                        ImGui::BulletText("bottom.jpg, front.jpg, back.jpg");
                        ImGui::InputText("Folder Path", skyboxPath, 256);
                        if (ImGui::Button("Load Cubemap")) {
                            skyboxNeedsReload = true;
                        }
                    } else if (skyboxMode == 2) {
                        ImGui::InputText("HDR File", skyboxPath, 256);
                        if (ImGui::Button("Load HDR")) {
                            skyboxNeedsReload = true;
                        }
                    }
                    
                    ImGui::Separator();
                    ImGui::Text("Skybox Lighting");
                    ImGui::Checkbox("Enable Skybox Lighting", &enableSkyboxLighting);
                    if (enableSkyboxLighting) {
                        ImGui::SliderFloat("Sky Light Intensity", &skyboxLightIntensity, 0.0f, 1.0f);
                    }
                }
                
                ImGui::Separator();
                ImGui::Text("Ambient Occlusion");
                ImGui::Checkbox("Enable AO", &enableAO);
                if (enableAO) {
                    ImGui::SliderFloat("AO Strength", &aoStrength, 0.0f, 1.0f);
                    ImGui::SliderFloat("AO Power", &aoPower, 0.5f, 5.0f);
                    ImGui::TextWrapped("AO darkens surfaces based on viewing angle. Higher power = sharper falloff.");
                }
                
                ImGui::Separator();
                ImGui::Text("PBR Materials");
                ImGui::Checkbox("Use PBR", &usePBR);
                if (usePBR) {
                    ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f);
                    ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f);
                    ImGui::SliderFloat("AO", &pbrAO, 0.0f, 1.0f);
                    ImGui::TextWrapped("PBR: Physically Based Rendering. Metallic=0 is dielectric, =1 is metal. Roughness=0 is smooth/shiny, =1 is rough/matte.");
                } else {
                    ImGui::TextWrapped("Using Blinn-Phong lighting (legacy)");
                }
                
                ImGui::Separator();
                ImGui::Text("Model Loading");
                ImGui::Checkbox("Show Model", &showModel);
                if (showModel) {
                    ImGui::InputText("Model Path", modelPath, 256);
                    if (ImGui::Button("Load GLB/GLTF")) {
                        if (loadedModel.loadModel(modelPath)) {
                            loadedModel.position = modelPosition;
                            loadedModel.rotation = modelRotation;
                            loadedModel.scale = modelScale;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear Model")) {
                        loadedModel.clear();
                    }
                    
                    if (loadedModel.loaded) {
                        ImGui::Text("Model loaded: %d meshes", (int)loadedModel.meshes.size());
                        ImGui::DragFloat3("Position", &modelPosition.x, 0.1f);
                        ImGui::DragFloat3("Rotation", &modelRotation.x, 1.0f);
                        ImGui::DragFloat3("Scale", &modelScale.x, 0.1f, 0.01f, 10.0f);
                        ImGui::ColorEdit3("Model Color", &modelColor.x);
                        
                        // Update model transform
                        loadedModel.position = modelPosition;
                        loadedModel.rotation = modelRotation;
                        loadedModel.scale = modelScale;
                    } else {
                        ImGui::TextWrapped("Place .glb or .gltf files in the models/ folder");
                    }
                }
            }
            
            ImGui::Separator();
            if (ImGui::Button("Reload Shaders")) {
                try {
                    depthShader = Shader("shaders/depth.vert", "shaders/depth.frag");
                    pbrShader = Shader("shaders/pbr.vert", "shaders/pbr.frag");
                    pbrShader.use();
                    pbrShader.setInt("shadowMap", 0);
                    pbrShader.setInt("shadowMap2", 1);
                    pbrShader.setInt("skybox", 2);
                    ImGui::Text("Shaders reloaded successfully!");
                } catch (const std::exception& e) {
                    ImGui::Text("Error reloading shaders!");
                }
            }
            
            ImGui::Separator();
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            
            ImGui::End();
        }

        // Apply animations
        if (animateLight) {
            float time = glfwGetTime() * animationSpeed;
            lightPos.x = cos(time) * 10.0f;
            lightPos.z = sin(time) * 10.0f;
        }
        
        if (animateCube) {
            cubeRotation.y = fmod(glfwGetTime() * 30.0f * animationSpeed, 360.0f);
        }

        // Enable/disable wireframe
        if (wireframeMode) {
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        } else {
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        }

        // 1. Render depth of scene to texture (from light's perspective)
        glm::mat4 lightProjection = glm::ortho(-lightOrthoSize, lightOrthoSize, -lightOrthoSize, lightOrthoSize, lightNear, lightFar);
        glm::mat4 lightView = glm::lookAt(lightPos, lightTarget, lightUp);
        glm::mat4 lightSpaceMatrix = lightProjection * lightView;
        glm::mat4 light2SpaceMatrix = glm::mat4(1.0f); // Initialize here

        depthShader.use();
        depthShader.setMat4("lightSpaceMatrix", lightSpaceMatrix);

        glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
        glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO);
        glClear(GL_DEPTH_BUFFER_BIT);

        // Render scene for depth map
        glm::mat4 model = glm::mat4(1.0f);
        depthShader.setMat4("model", model);
        glBindVertexArray(planeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        model = glm::mat4(1.0f);
        model = glm::translate(model, cubePosition);
        model = glm::rotate(model, glm::radians(cubeRotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, cubeScale);
        depthShader.setMat4("model", model);
        glBindVertexArray(cubeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 36);

        // Render second cube for depth map
        if (showSecondCube) {
            model = glm::mat4(1.0f);
            model = glm::translate(model, cube2Position);
            model = glm::rotate(model, glm::radians(cube2Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
            model = glm::rotate(model, glm::radians(cube2Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::rotate(model, glm::radians(cube2Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
            model = glm::scale(model, cube2Scale);
            depthShader.setMat4("model", model);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
        
        // Render loaded model for depth map
        if (showModel && loadedModel.loaded) {
            model = loadedModel.getModelMatrix();
            depthShader.setMat4("model", model);
            loadedModel.Draw();
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // 1b. Render depth of scene for second light (if enabled)
        if (enableSecondLight) {
            glm::mat4 light2Projection = glm::ortho(-lightOrthoSize, lightOrthoSize, -lightOrthoSize, lightOrthoSize, lightNear, lightFar);
            glm::mat4 light2View = glm::lookAt(light2Pos, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            light2SpaceMatrix = light2Projection * light2View;

            depthShader.use();
            depthShader.setMat4("lightSpaceMatrix", light2SpaceMatrix);

            glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
            glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO2);
            glClear(GL_DEPTH_BUFFER_BIT);

            // Render scene for second depth map
            glm::mat4 model = glm::mat4(1.0f);
            depthShader.setMat4("model", model);
            glBindVertexArray(planeVAO);
            glDrawArrays(GL_TRIANGLES, 0, 6);

            model = glm::mat4(1.0f);
            model = glm::translate(model, cubePosition);
            model = glm::rotate(model, glm::radians(cubeRotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
            model = glm::rotate(model, glm::radians(cubeRotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::rotate(model, glm::radians(cubeRotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
            model = glm::scale(model, cubeScale);
            depthShader.setMat4("model", model);
            glBindVertexArray(cubeVAO);
            glDrawArrays(GL_TRIANGLES, 0, 36);

            // Render second cube for depth map
            if (showSecondCube) {
                model = glm::mat4(1.0f);
                model = glm::translate(model, cube2Position);
                model = glm::rotate(model, glm::radians(cube2Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
                model = glm::rotate(model, glm::radians(cube2Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
                model = glm::rotate(model, glm::radians(cube2Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
                model = glm::scale(model, cube2Scale);
                depthShader.setMat4("model", model);
                glDrawArrays(GL_TRIANGLES, 0, 36);
            }
            
            // Render loaded model for depth map (second light)
            if (showModel && loadedModel.loaded) {
                model = loadedModel.getModelMatrix();
                depthShader.setMat4("model", model);
                loadedModel.Draw();
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // 2. Render scene as normal using the generated depth/shadow map
        glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
        glClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Use PBR shader
        pbrShader.use();
        glm::mat4 projection;
        if (projectionType == 0) {
            // Perspective projection
            projection = glm::perspective(glm::radians(cameraFOV), (float)SCR_WIDTH / (float)SCR_HEIGHT, cameraNear, cameraFar);
        } else {
            // Orthographic projection
            float aspect = (float)SCR_WIDTH / (float)SCR_HEIGHT;
            projection = glm::ortho(-orthoSize * aspect, orthoSize * aspect, -orthoSize, orthoSize, cameraNear, cameraFar);
        }
        glm::mat4 view = camera.GetViewMatrix();
        pbrShader.setMat4("projection", projection);
        pbrShader.setMat4("view", view);
        pbrShader.setMat4("lightSpaceMatrix", lightSpaceMatrix);
        pbrShader.setMat4("light2SpaceMatrix", light2SpaceMatrix);
        pbrShader.setVec3("viewPos", camera.Position);
        pbrShader.setVec3("lightPos", lightPos);
        
        // Set light properties
        pbrShader.setInt("lightType", lightType);
        pbrShader.setFloat("constant", lightConstant);
        pbrShader.setFloat("linear", lightLinear);
        pbrShader.setFloat("quadratic", lightQuadratic);
        pbrShader.setFloat("shadowBias", shadowBias);
        pbrShader.setBool("enableShadows", enableShadows);
        
        // Second light properties
        pbrShader.setBool("enableSecondLight", enableSecondLight);
        pbrShader.setVec3("light2Pos", light2Pos);
        pbrShader.setVec3("light2Color", light2Color);
        pbrShader.setFloat("light2Intensity", light2Intensity);
        
        // Skybox lighting properties
        pbrShader.setBool("enableSkyboxLighting", enableSkyboxLighting);
        pbrShader.setFloat("skyboxLightIntensity", skyboxLightIntensity);
        
        // Ambient Occlusion properties
        pbrShader.setBool("enableAO", enableAO);
        pbrShader.setFloat("aoStrength", aoStrength);
        pbrShader.setFloat("aoPower", aoPower);
        
        // PBR Material properties
        pbrShader.setBool("usePBR", usePBR);
        pbrShader.setFloat("metallic", metallic);
        pbrShader.setFloat("roughness", roughness);
        pbrShader.setFloat("ao", pbrAO);
        
        // Set texture samplers
        pbrShader.setInt("shadowMap", 0);
        pbrShader.setInt("shadowMap2", 1);
        pbrShader.setInt("skybox", 2);
        
        // Floor
        model = glm::mat4(1.0f);
        pbrShader.setMat4("model", model);
        pbrShader.setVec3("objectColor", floorColor);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depthMap);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthMap2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
    glBindVertexArray(planeVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);

        // Cube
        model = glm::mat4(1.0f);
        model = glm::translate(model, cubePosition);
        model = glm::rotate(model, glm::radians(cubeRotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, cubeScale);
        pbrShader.setMat4("model", model);
        pbrShader.setVec3("objectColor", cubeColor);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depthMap);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, depthMap2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
    glBindVertexArray(cubeVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);

        // Second Cube
        if (showSecondCube) {
            model = glm::mat4(1.0f);
            model = glm::translate(model, cube2Position);
            model = glm::rotate(model, glm::radians(cube2Rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
            model = glm::rotate(model, glm::radians(cube2Rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::rotate(model, glm::radians(cube2Rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
            model = glm::scale(model, cube2Scale);
            pbrShader.setMat4("model", model);
            pbrShader.setVec3("objectColor", cube2Color);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, depthMap);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, depthMap2);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
        
        // Loaded Model
        if (showModel && loadedModel.loaded) {
            model = loadedModel.getModelMatrix();
            pbrShader.setMat4("model", model);
            pbrShader.setVec3("objectColor", modelColor);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, depthMap);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, depthMap2);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
            loadedModel.Draw();
        }

        // Debug depth visualization
        if (renderMode == 1) {
            // Render shadow map depth as full screen
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            debugDepthShader.use();
            debugDepthShader.setFloat("near_plane", lightNear);
            debugDepthShader.setFloat("far_plane", lightFar);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, depthMap);
            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
        }
        
        // Shadow map overlay (bottom-right corner)
        if (showShadowMapOverlay && renderMode == 0) {
            // Disable depth test for overlay
            glDisable(GL_DEPTH_TEST);
            
            // Calculate overlay position and size
            float overlayWidth = overlaySize;
            float overlayHeight = overlaySize * ((float)SCR_WIDTH / (float)SCR_HEIGHT);
            float xPos = 1.0f - overlayWidth - 0.02f;  // 2% margin from right
            float yPos = -1.0f + overlayHeight + 0.02f; // 2% margin from bottom
            
            // Set viewport for overlay (bottom-right corner)
            int overlayPixelWidth = (int)(SCR_WIDTH * overlaySize);
            int overlayPixelHeight = (int)(SCR_HEIGHT * overlaySize);
            glViewport(SCR_WIDTH - overlayPixelWidth, 0, overlayPixelWidth, overlayPixelHeight);
            
            debugDepthShader.use();
            debugDepthShader.setFloat("near_plane", lightNear);
            debugDepthShader.setFloat("far_plane", lightFar);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, depthMap);
            glBindVertexArray(quadVAO);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            
            // Restore viewport and depth test
            glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
            glEnable(GL_DEPTH_TEST);
        }

        // Render skybox (last, with depth function LEQUAL)
        if (enableSkybox && renderMode == 0) {
            glDepthFunc(GL_LEQUAL);
            skyboxShader.use();
            glm::mat4 view = glm::mat4(glm::mat3(camera.GetViewMatrix())); // Remove translation
            glm::mat4 projection;
            if (projectionType == 0) {
                projection = glm::perspective(glm::radians(cameraFOV), (float)SCR_WIDTH / (float)SCR_HEIGHT, cameraNear, cameraFar);
            } else {
                float aspect = (float)SCR_WIDTH / (float)SCR_HEIGHT;
                projection = glm::ortho(-orthoSize * aspect, orthoSize * aspect, -orthoSize, orthoSize, cameraNear, cameraFar);
            }
            skyboxShader.setMat4("view", view);
            skyboxShader.setMat4("projection", projection);
            skyboxShader.setVec3("skyboxTint", skyboxTint);
            glBindVertexArray(skyboxVAO);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, skyboxTexture);
            glDrawArrays(GL_TRIANGLES, 0, 36);
            glBindVertexArray(0);
            glDepthFunc(GL_LESS);
        }

        // Render ImGui
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwTerminate();
    return 0;
}
