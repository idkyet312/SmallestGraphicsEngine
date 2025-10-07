# Shadow Mapping Graphics Engine

A minimal 3D graphics engine demonstrating shadow mapping with OpenGL, written in C++.

## Features
- Real-time shadow mapping with depth pass
- PCF (Percentage Closer Filtering) for soft shadows
- Phong lighting (ambient + diffuse + specular)
- Free-look camera with WASD movement
- Orthographic projection for directional light
- **ImGui interface** for real-time parameter editing
- **Shader hot-reload** for rapid development

## Project Structure
```
GraphicEngine/
├── src/
│   ├── main.cpp           # Main application loop with ImGui
│   ├── Shader.h           # Shader compilation and uniform helpers
│   └── Camera.h           # Camera movement and view matrix
├── shaders/
│   ├── depth.vert         # Depth pass vertex shader
│   ├── depth.frag         # Depth pass fragment shader
│   ├── shadow.vert        # Scene vertex shader with shadow coords
│   └── shadow.frag        # Scene fragment shader with shadow mapping
├── CMakeLists.txt         # CMake build configuration
├── build_and_run.ps1      # Full build and run script
├── quick_build.ps1        # Fast rebuild for code changes
└── README.md
```

## Controls
- **WASD**: Move camera
- **Mouse**: Look around
- **TAB**: Toggle ImGui UI
- **ESC**: Exit application

## ImGui Features
The UI allows you to edit in real-time:
- **Camera Settings**: Position, FOV, near/far planes, movement speed, mouse sensitivity
- **Light Settings**: Light position, orthographic projection parameters
- **Cube Settings**: Position, scale, color
- **Scene Settings**: Floor color, background color
- **Shader Reload**: Hot-reload shaders without restarting

## Prerequisites

### Windows (Recommended: vcpkg)
1. Install [vcpkg](https://github.com/microsoft/vcpkg):
   ```powershell
   git clone https://github.com/Microsoft/vcpkg.git
   cd vcpkg
   .\bootstrap-vcpkg.bat
   .\vcpkg integrate install
   ```

2. Install dependencies:
   ```powershell
   .\vcpkg install glfw3:x64-windows glm:x64-windows glad:x64-windows imgui[glfw-binding,opengl3-binding]:x64-windows
   ```

3. Install Visual Studio 2019/2022 with:
   - Desktop development with C++
   - C++ CMake tools for Windows

## Building

### Quick Start (PowerShell Scripts)

**Full build and run:**
```powershell
.\build_and_run.ps1
```

**Quick rebuild (after code changes):**
```powershell
.\quick_build.ps1
```

### Option 1: Using build.ps1 (Easiest)
```powershell
.\build.ps1
```

### Option 2: Manual CMake with vcpkg
```powershell
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

## Running
```powershell
.\build\Release\GraphicEngine.exe
```

Or from the project root (shaders are auto-copied):
```powershell
cd build
.\GraphicEngine.exe
```

## Controls
- **W/A/S/D**: Move camera forward/left/backward/right
- **Mouse**: Look around (cursor is captured)
- **ESC**: Exit

## How It Works

### Shadow Mapping Pipeline
1. **Depth Pass**: Render scene from light's perspective into a depth texture
2. **Scene Pass**: Render scene from camera with shadow lookups

### Key Concepts
- **Light-space matrix**: Transforms world coords → light clip space
- **Depth bias**: Prevents shadow acne (self-shadowing artifacts)
- **PCF**: 3×3 kernel averaging for softer shadow edges
- **Orthographic projection**: Used for directional lights (parallel rays)

## Troubleshooting

### CMake can't find dependencies
- Ensure vcpkg is integrated: `.\vcpkg integrate install`
- Pass toolchain file: `-DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake`

### Black screen or no shadows
- Check console for shader compilation errors
- Verify `shaders/` folder is in the working directory
- Ensure OpenGL 3.3+ support (check GPU drivers)

### Shadow artifacts
- **Shadow acne** (speckled self-shadows): increase bias in `shadow.frag`
- **Peter-panning** (detached shadows): decrease bias
- **Jagged edges**: increase `SHADOW_WIDTH`/`HEIGHT` in `main.cpp`

## Next Steps
- Add multiple lights or moving light
- Implement cascaded shadow maps for larger scenes
- Add normal mapping or PBR materials
- Support point/spot lights with perspective shadow maps