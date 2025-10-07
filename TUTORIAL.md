# Shadow Mapping Graphics Engine - Complete Tutorial

## What We Built
A complete 3D graphics engine in C++ that demonstrates real-time shadow mapping using OpenGL. The engine renders a simple scene (a cube on a plane) with dynamic shadows cast by a directional light.

## Core Concepts Explained

### 1. **Shadow Mapping Overview**
Shadow mapping is a two-pass rendering technique:
- **Pass 1 (Depth Pass)**: Render the scene from the light's perspective, storing only depth values in a texture
- **Pass 2 (Lighting Pass)**: Render the scene from the camera, comparing each fragment's depth against the light's depth map to determine if it's in shadow

### 2. **The Rendering Pipeline**

```
Frame Start
    │
    ├──> DEPTH PASS (Light's Perspective)
    │    │
    │    ├─ Bind depth-only FBO
    │    ├─ Set viewport to shadow map resolution (2048x2048)
    │    ├─ Use depth shader (depth.vert + depth.frag)
    │    ├─ Render all objects with lightSpaceMatrix
    │    └─ Depth values written to depthMap texture
    │
    └──> LIGHTING PASS (Camera's Perspective)
         │
         ├─ Bind default framebuffer (screen)
         ├─ Set viewport to window size (1280x720)
         ├─ Use shadow shader (shadow.vert + shadow.frag)
         ├─ Bind depthMap as texture unit 0
         ├─ Render all objects with camera matrices
         │    │
         │    └─ For each fragment:
         │         ├─ Transform position to light space
         │         ├─ Sample depth map (with PCF)
         │         ├─ Compare depths (with bias)
         │         └─ Apply shadow factor to lighting
         │
         └─ Swap buffers
```

### 3. **Key Matrix Transformations**

#### Light-Space Matrix
```cpp
glm::mat4 lightProjection = glm::ortho(-15.0f, 15.0f, -15.0f, 15.0f, 1.0f, 25.0f);
glm::mat4 lightView = glm::lookAt(lightPos, vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
glm::mat4 lightSpaceMatrix = lightProjection * lightView;
```
- **lightProjection**: Orthographic projection for directional light (parallel rays)
- **lightView**: Positions and orients the "light camera"
- **lightSpaceMatrix**: Combined transform (world → light clip space)

#### Camera Matrices
```cpp
glm::mat4 projection = glm::perspective(radians(45.0f), aspect, 0.1f, 100.0f);
glm::mat4 view = camera.GetViewMatrix();
```
- **projection**: Perspective projection for realistic depth
- **view**: Camera position and orientation

#### Transform Order
```
Vertex (object space)
  → model matrix
  → World space
    → view matrix
    → Camera space
      → projection matrix
      → Clip space
        → perspective divide
        → NDC (Normalized Device Coordinates)
          → viewport transform
          → Screen space
```

### 4. **Shadow Calculation (in shadow.frag)**

```glsl
float ShadowCalculation(vec4 fragPosLightSpace) {
    // 1. Perspective divide
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    
    // 2. Map from NDC [-1,1] to texture coords [0,1]
    projCoords = projCoords * 0.5 + 0.5;
    
    // 3. Get current fragment depth
    float currentDepth = projCoords.z;
    
    // 4. Calculate angle-dependent bias (prevents shadow acne)
    vec3 normal = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float bias = max(0.05 * (1.0 - dot(normal, lightDir)), 0.005);
    
    // 5. PCF (Percentage Closer Filtering) for soft shadows
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;  // Average of 9 samples
    
    // 6. Fragments outside light frustum are not in shadow
    if(projCoords.z > 1.0) shadow = 0.0;
    
    return shadow;  // 0.0 = fully lit, 1.0 = fully shadowed
}
```

### 5. **Phong Lighting Model**

```glsl
void main() {
    // Ambient: constant base lighting
    vec3 ambient = 0.3 * objectColor;
    
    // Diffuse: angle between surface normal and light direction
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(lightDir, normal), 0.0);
    vec3 diffuse = diff * lightColor;
    
    // Specular: reflection highlights
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    vec3 specular = spec * lightColor;
    
    // Combine with shadow factor
    float shadow = ShadowCalculation(FragPosLightSpace);
    vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular)) * objectColor;
    
    FragColor = vec4(lighting, 1.0);
}
```

**Key insight**: Only diffuse and specular are affected by shadows; ambient provides base illumination even in shadow.

### 6. **Framebuffer Objects (FBO)**

```cpp
// Create depth texture
glGenTextures(1, &depthMap);
glBindTexture(GL_TEXTURE_2D, depthMap);
glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 
             SHADOW_WIDTH, SHADOW_HEIGHT, 0, 
             GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

// Create and configure FBO
glGenFramebuffers(1, &depthMapFBO);
glBindFramebuffer(GL_FRAMEBUFFER, depthMapFBO);
glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthMap, 0);
glDrawBuffer(GL_NONE);  // No color output
glReadBuffer(GL_NONE);
```

**Why border color = 1.0?** Fragments outside the light frustum sample the border, which should be "far" (no occluder) so they're not shadowed.

### 7. **Common Artifacts & Solutions**

#### Shadow Acne (speckled self-shadowing)
**Cause**: Depth precision limitations cause surfaces to shadow themselves  
**Solution**: Add a bias before depth comparison
```glsl
float bias = max(0.05 * (1.0 - dot(normal, lightDir)), 0.005);
shadow = currentDepth - bias > pcfDepth ? 1.0 : 0.0;
```

#### Peter-Panning (detached shadows)
**Cause**: Bias too large, shadows "float" away from objects  
**Solution**: Reduce bias or use normal offset bias

#### Jagged Shadow Edges
**Cause**: Low shadow map resolution or no filtering  
**Solution**: 
- Increase SHADOW_WIDTH/HEIGHT
- Use PCF (already implemented)
- Use higher-order filtering (VSM, ESM, etc.)

### 8. **VAO/VBO Setup Pattern**

```cpp
// 1. Generate and bind VAO
unsigned int VAO, VBO;
glGenVertexArrays(1, &VAO);
glGenBuffers(1, &VBO);
glBindVertexArray(VAO);

// 2. Upload vertex data
glBindBuffer(GL_ARRAY_BUFFER, VBO);
glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

// 3. Configure vertex attributes (must match shader layout locations)
// layout(location = 0) in vec3 aPos;
glEnableVertexAttribArray(0);
glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);

// layout(location = 1) in vec3 aNormal;
glEnableVertexAttribArray(1);
glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));

// 4. Unbind
glBindVertexArray(0);
```

**Critical**: Attribute location indices (0, 1) must match `layout(location = N)` in GLSL.

### 9. **Camera System**

```cpp
class Camera {
    glm::vec3 Position, Front, Up;
    float Yaw, Pitch;
    
    glm::mat4 GetViewMatrix() {
        return glm::lookAt(Position, Position + Front, Up);
    }
    
    void ProcessKeyboard(char direction, float deltaTime) {
        float velocity = 2.5f * deltaTime;
        if (direction == 'W') Position += Front * velocity;
        // ... WASD movement
    }
    
    void ProcessMouseMovement(float xoffset, float yoffset) {
        Yaw += xoffset * 0.1f;
        Pitch += yoffset * 0.1f;
        Pitch = clamp(Pitch, -89.0f, 89.0f);  // Prevent gimbal lock
        updateCameraVectors();  // Recalculate Front from Yaw/Pitch
    }
};
```

### 10. **Shader Compilation Pattern**

```cpp
class Shader {
    unsigned int ID;
    
    Shader(const char* vertPath, const char* fragPath) {
        // 1. Read source from files
        std::string vertCode = readFile(vertPath);
        std::string fragCode = readFile(fragPath);
        
        // 2. Compile vertex shader
        unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertex, 1, &vertCode, NULL);
        glCompileShader(vertex);
        checkCompileErrors(vertex, "VERTEX");
        
        // 3. Compile fragment shader
        unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment, 1, &fragCode, NULL);
        glCompileShader(fragment);
        checkCompileErrors(fragment, "FRAGMENT");
        
        // 4. Link program
        ID = glCreateProgram();
        glAttachShader(ID, vertex);
        glAttachShader(ID, fragment);
        glLinkProgram(ID);
        checkCompileErrors(ID, "PROGRAM");
        
        // 5. Clean up
        glDeleteShader(vertex);
        glDeleteShader(fragment);
    }
    
    void setMat4(const std::string &name, const glm::mat4 &mat) {
        glUniformMatrix4fv(glGetUniformLocation(ID, name.c_str()), 
                          1, GL_FALSE, &mat[0][0]);
    }
};
```

## Performance Considerations

1. **Shadow Map Resolution**: 2048×2048 is a good balance
   - Higher = sharper shadows, more VRAM
   - Lower = faster, but more aliasing

2. **PCF Kernel Size**: 3×3 (9 samples)
   - Larger kernel = softer shadows, more expensive
   - Consider variable kernel based on distance

3. **Depth Precision**: Use GL_DEPTH_COMPONENT24 or 32
   - More bits = better precision, less z-fighting

4. **Framerate Independence**: Always use `deltaTime`
   ```cpp
   camera.ProcessKeyboard('W', deltaTime);  // Speed independent of FPS
   ```

## Advanced Topics (Next Steps)

1. **Cascaded Shadow Maps (CSM)**: Multiple shadow maps for different depth ranges
2. **Omnidirectional Shadows**: Cubemap depth for point lights
3. **Soft Shadows**: PCSS (Percentage Closer Soft Shadows)
4. **Shadow Atlas**: Multiple lights in one texture
5. **Temporal Filtering**: Accumulate shadow samples over frames

## Files Summary

| File | Purpose |
|------|---------|
| `src/main.cpp` | Main application loop, setup, and render passes |
| `src/Shader.h` | GLSL shader compilation and uniform management |
| `src/Camera.h` | Free-look camera with WASD + mouse |
| `shaders/depth.vert` | Transforms vertices into light space |
| `shaders/depth.frag` | Depth-only output (auto) |
| `shaders/shadow.vert` | Transforms vertices + computes shadow coords |
| `shaders/shadow.frag` | Phong lighting + shadow mapping with PCF |
| `CMakeLists.txt` | Build configuration |
| `build.ps1` | Automated build script (Windows) |

## Building and Running

```powershell
# One-command build
.\build.ps1

# Run
.\build\GraphicEngine.exe
```

## Controls
- **WASD**: Move camera
- **Mouse**: Look around
- **ESC**: Exit

## Learning Resources
- LearnOpenGL Shadow Mapping: https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping
- OpenGL SuperBible (7th Edition): Chapter 12
- Real-Time Rendering (4th Edition): Chapter 7

---

**Congratulations!** You now have a working shadow mapping engine and understand the core concepts of real-time shadow rendering.
