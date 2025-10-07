#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include "Shader.h"
#include "Camera.h"

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
    }
    float xoffset = (float)xpos - lastX;
    float yoffset = lastY - (float)ypos; // reversed
    lastX = (float)xpos;
    lastY = (float)ypos;
    camera.ProcessMouseMovement(xoffset, yoffset);
}

void processInput(GLFWwindow *window) {
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
    
    // Toggle UI with Tab key
    static bool tabPressed = false;
    if (glfwGetKey(window, GLFW_KEY_TAB) == GLFW_PRESS && !tabPressed) {
        showUI = !showUI;
        glfwSetInputMode(window, GLFW_CURSOR, showUI ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
        tabPressed = true;
    }
    if (glfwGetKey(window, GLFW_KEY_TAB) == GLFW_RELEASE) {
        tabPressed = false;
    }
    
    // Toggle camera lock with C key
    static bool cPressed = false;
    if (glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS && !cPressed) {
        cameraLocked = !cameraLocked;
        if (cameraLocked) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (!showUI) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }
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
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

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
    Shader debugDepthShader("shaders/debug_depth.vert", "shaders/debug_depth.frag");

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

    unsigned int cubeVAO = loadCubeVAO();
    unsigned int planeVAO = loadPlaneVAO();
    unsigned int quadVAO = loadQuadVAO();

    shadowShader.use();
    shadowShader.setInt("shadowMap", 0);
    
    debugDepthShader.use();
    debugDepthShader.setInt("depthMap", 0);

    while (!glfwWindowShouldClose(window)) {
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        processInput(window);

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
                ImGui::Text("Light Transform");
                ImGui::DragFloat3("Light Position", &lightPos.x, 0.1f);
                ImGui::DragFloat3("Light Target", &lightTarget.x, 0.1f);
                ImGui::DragFloat3("Light Up Vector", &lightUp.x, 0.01f);
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
            }
            
            ImGui::Separator();
            if (ImGui::Button("Reload Shaders")) {
                try {
                    depthShader = Shader("shaders/depth.vert", "shaders/depth.frag");
                    shadowShader = Shader("shaders/shadow.vert", "shaders/shadow.frag");
                    shadowShader.use();
                    shadowShader.setInt("shadowMap", 0);
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

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // 2. Render scene as normal using the generated depth/shadow map
        glViewport(0, 0, SCR_WIDTH, SCR_HEIGHT);
        glClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        shadowShader.use();
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
        shadowShader.setMat4("projection", projection);
        shadowShader.setMat4("view", view);
        shadowShader.setMat4("lightSpaceMatrix", lightSpaceMatrix);
        shadowShader.setVec3("viewPos", camera.Position);
        shadowShader.setVec3("lightPos", lightPos);

        // Floor
        model = glm::mat4(1.0f);
        shadowShader.setMat4("model", model);
        shadowShader.setVec3("objectColor", floorColor);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, depthMap);
        glBindVertexArray(planeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Cube
        model = glm::mat4(1.0f);
        model = glm::translate(model, cubePosition);
        model = glm::rotate(model, glm::radians(cubeRotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, cubeScale);
        shadowShader.setMat4("model", model);
        shadowShader.setVec3("objectColor", cubeColor);
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
            shadowShader.setMat4("model", model);
            shadowShader.setVec3("objectColor", cube2Color);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        // Debug depth visualization
        if (renderMode == 1) {
            // Render shadow map depth as overlay or full screen
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
