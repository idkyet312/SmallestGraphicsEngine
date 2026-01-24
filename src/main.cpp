#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include "Shader.h"
#include "Camera.h"
#include "Model.h"
#include "ClusteredRenderer.h"

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

// Viewmodel (gun attached to camera)
Model gunModel;

// Projectile system
struct Projectile {
    glm::vec3 position;
    glm::vec3 direction;
    float speed;
    float lifetime;
    bool active;
};
std::vector<Projectile> projectiles;
float projectileSpeed = 50.0f;
float projectileLifetime = 3.0f;
glm::vec3 projectileColor(1.0f, 0.8f, 0.0f);
float projectileScale = 0.1f;

// Clustered Forward Rendering
ClusteredRenderer clusteredRenderer;
bool useClusteredRendering = true;
bool showClusterDebug = false;
int numDemoLights = 8;
float demoLightRadius = 8.0f;
float demoLightIntensity = 1.5f;
bool animateDemoLights = true;

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
            if (cameraLocked) {
                // Lock cursor to window
                cameraLocked = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                // Reset cursor position to center
                glfwSetCursorPos(window, SCR_WIDTH / 2.0, SCR_HEIGHT / 2.0);
                lastX = SCR_WIDTH / 2.0f;
                lastY = SCR_HEIGHT / 2.0f;
                firstMouse = true;
            } else {
                // Shoot a projectile
                Projectile proj;
                proj.position = camera.Position + camera.Front * 0.5f; // Start slightly in front of camera
                proj.direction = glm::normalize(camera.Front);
                proj.speed = projectileSpeed;
                proj.lifetime = projectileLifetime;
                proj.active = true;
                projectiles.push_back(proj);
            }
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
    
    // Toggle FPS walking mode with F key
    static bool fPressed = false;
    if (glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS && !fPressed) {
        camera.FPSMode = !camera.FPSMode;
        if (camera.FPSMode) {
            // Snap camera to floor height when entering FPS mode
            camera.Position.y = camera.FloorY + camera.PlayerHeight;
        }
        fPressed = true;
    }
    if (glfwGetKey(window, GLFW_KEY_F) == GLFW_RELEASE) {
        fPressed = false;
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
        
        // Jump with Space bar
        static bool spacePressed = false;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS && !spacePressed) {
            camera.Jump();
            spacePressed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_RELEASE) {
            spacePressed = false;
        }
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
    Shader clusteredShader("shaders/clustered.vert", "shaders/clustered.frag");
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
    
    // Load gun model
    gunModel.loadOBJ("models/gun.obj");
    gunModel.offset = glm::vec3(0.170f, -0.140f, 0.490f);
    gunModel.scale = glm::vec3(0.5f);
    gunModel.rotation = glm::vec3(0.0f, 180.0f, 0.0f);
    gunModel.color = glm::vec3(0.2f, 0.2f, 0.25f);

    shadowShader.use();
    shadowShader.setInt("shadowMap", 0);
    
    // Set default light attenuation values
    shadowShader.setFloat("constant", lightConstant);
    shadowShader.setFloat("linear", lightLinear);
    shadowShader.setFloat("quadratic", lightQuadratic);
    
    debugDepthShader.use();
    debugDepthShader.setInt("depthMap", 0);

    while (!glfwWindowShouldClose(window)) {
        float currentFrame = glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        processInput(window);
        
        // Update camera physics (gravity, ground collision) every frame
        camera.Update(deltaTime);

        // Update projectiles
        for (auto& proj : projectiles) {
            if (proj.active) {
                proj.position += proj.direction * proj.speed * deltaTime;
                proj.lifetime -= deltaTime;
                if (proj.lifetime <= 0.0f) {
                    proj.active = false;
                }
            }
        }
        // Remove inactive projectiles
        projectiles.erase(
            std::remove_if(projectiles.begin(), projectiles.end(),
                [](const Projectile& p) { return !p.active; }),
            projectiles.end());

        // Update demo lights animation
        if (animateDemoLights && useClusteredRendering) {
            float time = (float)glfwGetTime() * animationSpeed;
            for (int i = 0; i < clusteredRenderer.getLightCount(); i++) {
                float angle = (float)i / clusteredRenderer.getLightCount() * 6.28318f + time;
                float radius = 8.0f;
                clusteredRenderer.lights[i].position = glm::vec3(
                    cos(angle) * radius,
                    2.0f + sin(time * 2.0f + (float)i) * 1.5f,
                    sin(angle) * radius
                );
            }
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
            ImGui::BulletText("F: Toggle FPS Walking Mode");
            ImGui::BulletText("Left Click: Lock camera / Shoot");
            
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
                ImGui::Text("Camera Mode");
                if (ImGui::Checkbox("FPS Walking Mode", &camera.FPSMode)) {
                    if (camera.FPSMode) {
                        // When entering FPS mode, snap camera to floor height
                        camera.Position.y = camera.FloorY + camera.PlayerHeight;
                    }
                }
                if (camera.FPSMode) {
                    ImGui::DragFloat("Player Height", &camera.PlayerHeight, 0.05f, 0.5f, 3.0f);
                    ImGui::DragFloat("Floor Y", &camera.FloorY, 0.1f, -10.0f, 10.0f);
                    ImGui::Text("Movement: WASD (locked to floor)");
                } else {
                    ImGui::Text("Movement: WASD (free fly)");
                }
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
                
                // Clustered Forward Rendering
                ImGui::Text("Rendering Pipeline");
                ImGui::Checkbox("Use Clustered Forward", &useClusteredRendering);
                if (useClusteredRendering) {
                    ImGui::Indent();
                    ImGui::Text("Cluster Grid: %dx%dx%d", ClusteredRenderer::CLUSTER_X, ClusteredRenderer::CLUSTER_Y, ClusteredRenderer::CLUSTER_Z);
                    ImGui::Text("Active Clusters: %d", clusteredRenderer.getActiveClusterCount());
                    ImGui::Text("Total Lights: %d", clusteredRenderer.getLightCount());
                    ImGui::Checkbox("Show Cluster Debug", &showClusterDebug);
                    ImGui::Separator();
                    
                    ImGui::Text("Demo Lights");
                    static int prevNumLights = numDemoLights;
                    ImGui::SliderInt("Number of Lights", &numDemoLights, 0, 64);
                    ImGui::DragFloat("Light Radius", &demoLightRadius, 0.5f, 1.0f, 50.0f);
                    ImGui::DragFloat("Light Intensity", &demoLightIntensity, 0.1f, 0.1f, 10.0f);
                    ImGui::Checkbox("Animate Lights", &animateDemoLights);
                    
                    // Update light properties
                    for (int i = 0; i < clusteredRenderer.getLightCount(); i++) {
                        clusteredRenderer.lights[i].radius = demoLightRadius;
                        clusteredRenderer.lights[i].intensity = demoLightIntensity;
                        // Recalculate attenuation
                        clusteredRenderer.lights[i].linear = 4.5f / demoLightRadius;
                        clusteredRenderer.lights[i].quadratic = 75.0f / (demoLightRadius * demoLightRadius);
                    }
                    
                    if (numDemoLights != prevNumLights) {
                        // Regenerate lights
                        clusteredRenderer.clearLights();
                        for (int i = 0; i < numDemoLights; i++) {
                            float angle = (float)i / numDemoLights * 6.28318f;
                            float radius = 8.0f;
                            glm::vec3 pos(cos(angle) * radius, 2.0f, sin(angle) * radius);
                            glm::vec3 color;
                            color.r = sin(angle) * 0.5f + 0.5f;
                            color.g = sin(angle + 2.094f) * 0.5f + 0.5f;
                            color.b = sin(angle + 4.189f) * 0.5f + 0.5f;
                            clusteredRenderer.addLight(pos, color, demoLightRadius, demoLightIntensity);
                        }
                        prevNumLights = numDemoLights;
                    }
                    
                    if (ImGui::Button("Add Light at Camera")) {
                        clusteredRenderer.addLight(camera.Position, glm::vec3(1.0f), demoLightRadius, demoLightIntensity);
                        numDemoLights = clusteredRenderer.getLightCount();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear All Lights")) {
                        clusteredRenderer.clearLights();
                        numDemoLights = 0;
                    }
                    
                    ImGui::Unindent();
                }
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
            }
            
            ImGui::Separator();
            // Viewmodel (Gun) settings
            if (ImGui::CollapsingHeader("Viewmodel (Gun)")) {
                ImGui::Checkbox("Show Gun", &gunModel.visible);
                ImGui::ColorEdit3("Gun Color", &gunModel.color[0]);
                ImGui::DragFloat3("Gun Offset", &gunModel.offset[0], 0.01f, -2.0f, 2.0f);
                ImGui::DragFloat3("Gun Scale", &gunModel.scale[0], 0.01f, 0.01f, 5.0f);
                ImGui::DragFloat3("Gun Rotation", &gunModel.rotation[0], 1.0f, -180.0f, 180.0f);
                
                ImGui::Separator();
                ImGui::Text("Projectile Settings");
                ImGui::ColorEdit3("Projectile Color", &projectileColor[0]);
                ImGui::DragFloat("Projectile Speed", &projectileSpeed, 1.0f, 1.0f, 200.0f);
                ImGui::DragFloat("Projectile Lifetime", &projectileLifetime, 0.1f, 0.1f, 10.0f);
                ImGui::DragFloat("Projectile Size", &projectileScale, 0.01f, 0.01f, 1.0f);
                ImGui::Text("Active Projectiles: %d", (int)projectiles.size());
                if (ImGui::Button("Clear Projectiles")) {
                    projectiles.clear();
                }
                
                ImGui::Separator();
                static char modelPathBuffer[256] = "models/gun.obj";
                ImGui::InputText("Model Path", modelPathBuffer, sizeof(modelPathBuffer));
                if (ImGui::Button("Load Model")) {
                    gunModel.loadOBJ(modelPathBuffer);
                }
            }

            if (ImGui::Button("Reload Shaders")) {
                try {
                    depthShader = Shader("shaders/depth.vert", "shaders/depth.frag");
                    shadowShader = Shader("shaders/shadow.vert", "shaders/shadow.frag");
                    clusteredShader = Shader("shaders/clustered.vert", "shaders/clustered.frag");
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

        // Update clustered renderer and cull lights
        glm::mat4 projection;
        if (projectionType == 0) {
            projection = glm::perspective(glm::radians(cameraFOV), (float)SCR_WIDTH / (float)SCR_HEIGHT, cameraNear, cameraFar);
        } else {
            float aspect = (float)SCR_WIDTH / (float)SCR_HEIGHT;
            projection = glm::ortho(-orthoSize * aspect, orthoSize * aspect, -orthoSize, orthoSize, cameraNear, cameraFar);
        }
        glm::mat4 view = camera.GetViewMatrix();

        // Update clustered renderer
        clusteredRenderer.setScreenSize((float)SCR_WIDTH, (float)SCR_HEIGHT);
        clusteredRenderer.setCamera(cameraFOV, cameraNear, cameraFar, view, projection);
        if (useClusteredRendering) {
            clusteredRenderer.cullLights();
        }

        // Choose shader based on rendering mode
        Shader& activeShader = useClusteredRendering ? clusteredShader : shadowShader;
        activeShader.use();
        
        activeShader.setMat4("projection", projection);
        activeShader.setMat4("view", view);
        activeShader.setMat4("lightSpaceMatrix", lightSpaceMatrix);
        activeShader.setVec3("viewPos", camera.Position);
        activeShader.setVec3("lightPos", lightPos);
        
        // Set light properties
        activeShader.setInt("lightType", lightType);
        activeShader.setVec3("lightColor", lightColor);
        activeShader.setFloat("constant", lightConstant);
        activeShader.setFloat("linear", lightLinear);
        activeShader.setFloat("quadratic", lightQuadratic);
        activeShader.setFloat("ambientStrength", ambientStrength);
        activeShader.setFloat("specularStrength", specularStrength);
        activeShader.setInt("shininess", specularShininess);
        activeShader.setFloat("shadowBias", shadowBias);
        activeShader.setBool("enableShadows", enableShadows);

        // Point lights uniforms (for clustered rendering)
        if (useClusteredRendering) {
            int numLights = std::min(clusteredRenderer.getLightCount(), 32);
            activeShader.setInt("numPointLights", numLights);
            
            for (int i = 0; i < numLights; i++) {
                const auto& light = clusteredRenderer.lights[i];
                std::string base = "pointLightPositions[" + std::to_string(i) + "]";
                activeShader.setVec3(base, light.position);
                
                base = "pointLightColors[" + std::to_string(i) + "]";
                activeShader.setVec3(base, light.color);
                
                base = "pointLightRadii[" + std::to_string(i) + "]";
                activeShader.setFloat(base, light.radius);
                
                base = "pointLightIntensities[" + std::to_string(i) + "]";
                activeShader.setFloat(base, light.intensity);
            }
            
            activeShader.setInt("shadowMap", 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, depthMap);
        } else {
            activeShader.setInt("numPointLights", 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, depthMap);
        }
        
        // Floor
        model = glm::mat4(1.0f);
        activeShader.setMat4("model", model);
        activeShader.setVec3("objectColor", floorColor);
        glBindVertexArray(planeVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        // Cube
        model = glm::mat4(1.0f);
        model = glm::translate(model, cubePosition);
        model = glm::rotate(model, glm::radians(cubeRotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(cubeRotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, cubeScale);
        activeShader.setMat4("model", model);
        activeShader.setVec3("objectColor", cubeColor);
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
            activeShader.setMat4("model", model);
            activeShader.setVec3("objectColor", cube2Color);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }

        // Render point light spheres (visual indicators)
        if (useClusteredRendering) {
            for (int i = 0; i < clusteredRenderer.getLightCount(); i++) {
                const auto& light = clusteredRenderer.lights[i];
                if (!light.active) continue;
                model = glm::mat4(1.0f);
                model = glm::translate(model, light.position);
                model = glm::scale(model, glm::vec3(0.2f));
                activeShader.setMat4("model", model);
                activeShader.setVec3("objectColor", light.color * light.intensity);
                glBindVertexArray(cubeVAO);
                glDrawArrays(GL_TRIANGLES, 0, 36);
            }
        }

        // Render projectiles
        for (const auto& proj : projectiles) {
            if (proj.active) {
                model = glm::mat4(1.0f);
                model = glm::translate(model, proj.position);
                model = glm::scale(model, glm::vec3(projectileScale));
                activeShader.setMat4("model", model);
                activeShader.setVec3("objectColor", projectileColor);
                glBindVertexArray(cubeVAO);
                glDrawArrays(GL_TRIANGLES, 0, 36);
            }
        }

        // Render gun viewmodel (attached to camera)
        if (gunModel.loaded && gunModel.visible) {
            glClear(GL_DEPTH_BUFFER_BIT);  // Clear depth so gun renders on top
            model = gunModel.getModelMatrix(camera.Position, camera.Front, camera.Up);
            activeShader.setMat4("model", model);
            activeShader.setVec3("objectColor", gunModel.color);
            gunModel.draw();
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
