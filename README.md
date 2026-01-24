# Clustered Forward Rendering Engine with DDGI

A DirectX 12 graphics engine featuring clustered forward rendering, Dynamic Diffuse Global Illumination (DDGI), and FPS gameplay mechanics.

## Features
- **Clustered Forward Rendering**: Efficient lighting with up to 64 lights
- **Dynamic Diffuse Global Illumination (DDGI)**: Real-time indirect lighting
- **DirectX 12 Renderer**: Modern graphics API with compute shader support
- **FPS Camera System**: Mouse-locked first-person controls
- **Interactive Shooting**: Target practice with damage system
- **Light Placement**: Dynamically add and move lights with gizmos
- **Gun Model**: Attached FPS weapon that follows camera rotation
- **ImGui Interface**: Real-time parameter editing
- **Uncapped Frame Rate**: High-performance rendering

## Project Structure
```
SmallestGraphicsEngine/
├── src/                          # Source code directory
│   ├── main_dx12.cpp             # Main DX12 application with clustered rendering
│   ├── main_dx11.cpp             # Legacy DX11 version
│   ├── DX12Core.h                # DirectX 12 initialization and device management
│   ├── DX11Core.h                # DirectX 11 core (legacy)
│   ├── CameraDX12.h              # FPS camera with mouse lock
│   ├── ShaderDX12.h              # DX12 shader compilation and root signatures
│   ├── ShaderDX11.h              # DX11 shader wrapper (legacy)
│   ├── ClusteredRendererDX12.h   # Clustered forward rendering implementation
│   ├── ClusteredRendererDX11.h   # DX11 clustered renderer (legacy)
│   ├── DDGI_DX11.h               # Dynamic Diffuse Global Illumination
│   ├── Model.h                   # OBJ model loader
│   └── ModelDX11.h               # DX11 model renderer
├── shaders/                      # HLSL shader files
│   ├── clustered_dx12_vs.hlsl    # DX12 clustered vertex shader
│   ├── clustered_dx12_ps.hlsl    # DX12 clustered pixel shader with DDGI
│   ├── clustered_vs.hlsl         # DX11 clustered vertex shader
│   ├── clustered_ps.hlsl         # DX11 clustered pixel shader
│   ├── depth_vs.hlsl             # Depth pass vertex shader
│   ├── depth_ps.hlsl             # Depth pass pixel shader
│   ├── debug_depth_vs.hlsl       # Debug visualization shaders
│   └── debug_depth_ps.hlsl
├── models/                       # 3D model assets
│   └── gun.obj                   # FPS gun model
├── build/                        # Build output directory
├── CMakeLists.txt                # CMake build configuration (DX11/DX12 option)
├── build.ps1                     # Build script
├── build_and_run.ps1             # Full build and run script
├── quick_build.ps1               # Fast rebuild for code changes
└── README.md                     # This file
```

## Controls
- **WASD**: Move camera (continuous movement, no jump)
- **Mouse**: Look around (locked to screen, resets to center)
- **Left Mouse Button**: Shoot (damages center cube)
- **L**: Place a new light at camera position
- **Arrow Keys**: Move selected light (Up/Down/Left/Right)
- **TAB**: Toggle ImGui UI
- **ESC**: Exit application

## Gameplay Features
- **FPS Gun**: First-person weapon attached to camera with realistic offset
- **Shooting System**: Left-click to shoot bullets that damage the center cube
- **Damage System**: Center cube has 100 health, turns red when hit, shows health in UI
- **Light Placement**: Press L to spawn lights, use arrow keys to move them with visual gizmos
- **64 Dynamic Lights**: Supports up to 64 lights with clustered forward rendering

## ImGui Features
The UI allows you to edit in real-time:
- **Camera Settings**: Position, rotation, FOV, movement speed, mouse sensitivity
- **Light Management**: Add/remove lights, adjust positions, colors, and intensities
- **Cube Health**: View and reset target cube health
- **Gun Settings**: Toggle visibility, adjust offset and scale
- **DDGI Settings**: Global illumination intensity and parameters
- **Rendering Stats**: FPS counter (uncapped frame rate)
- **Scene Settings**: Background color, floor properties

## Prerequisites

### Windows Requirements
- **Windows 10** or later (DirectX 12 support required)
- **DirectX 12 compatible GPU** (NVIDIA GTX 900 series or newer, AMD GCN architecture or newer)
- **Visual Studio 2019/2022** with:
  - Desktop development with C++
  - C++ CMake tools for Windows
  - Windows 10 SDK

### vcpkg Setup
1. Install [vcpkg](https://github.com/microsoft/vcpkg):
   ```powershell
   git clone https://github.com/Microsoft/vcpkg.git
   cd vcpkg
   .\bootstrap-vcpkg.bat
   .\vcpkg integrate install
   ```

2. Install dependencies:
   ```powershell
   # For DirectX 12 (default)
   .\vcpkg install imgui[core,dx12-binding,win32-binding]:x64-windows
   
   # For DirectX 11 (legacy)
   .\vcpkg install imgui[core,dx11-binding,win32-binding]:x64-windows
   ```

## Building

### Quick Start (PowerShell Scripts)

**Full build and run (DirectX 12):**
```powershell
.\build_and_run.ps1
```

**Quick rebuild (after code changes):**
```powershell
.\quick_build.ps1
```

### Option 1: Using build.ps1 (Easiest)
```powershell
# Build with DirectX 12 (default)
.\build.ps1

# Build with DirectX 11 (legacy)
.\build.ps1 -UseDX11
```

### Option 2: Manual CMake with vcpkg
```powershell
mkdir build
cd build

# For DirectX 12 (default)
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake -DUSE_DX12=ON
cmake --build . --config Release

# For DirectX 11 (legacy)
cmake .. -DCMAKE_TOOLCHAIN_FILE=[path-to-vcpkg]/scripts/buildsystems/vcpkg.cmake -DUSE_DX12=OFF
cmake --build . --config Release
```

## Running
```powershell
.\build\GraphicEngine.exe
```

Or from the build directory:
```powershell
cd build
.\GraphicEngine.exe
```

The executable will automatically load shaders from `build/shaders/` and models from `build/models/`.

## How It Works

### Clustered Forward Rendering Pipeline
1. **Cluster Grid Generation**: Divide view frustum into 3D grid of clusters (16x9x24)
2. **Light Assignment**: Compute shader assigns lights to clusters based on overlap
3. **Geometry Pass**: Render scene, each pixel queries its cluster for affecting lights
4. **DDGI Integration**: Dynamic Diffuse Global Illumination adds indirect lighting

### DirectX 12 Architecture
The engine uses modern DirectX 12 features:
- **Command Lists**: Pre-recorded GPU commands for efficient submission
- **Descriptor Heaps**: CBV/SRV/UAV management for shader resources
- **Root Signatures**: Define shader parameter layouts
- **Compute Shaders**: Light culling and cluster assignment
- **Resource Barriers**: Synchronize resource state transitions
- **Constant Buffers**: Per-frame data (camera, lights, settings)

### Key Rendering Concepts
- **Clustered Forward**: Divides screen into tiles in 3D space, assigns lights per cluster
- **DDGI (Dynamic Diffuse Global Illumination)**: Real-time indirect lighting using probes
- **64 Light Support**: Efficiently handles many lights without forward+ overhead
- **FPS Camera**: Mouse-locked camera with continuous WASD movement
- **Model Attachment**: Gun follows camera transform with custom offset
- **Bullet System**: Raycasting from camera for hit detection

### Performance
- **Uncapped Frame Rate**: No vsync or frame limiting
- **Efficient Light Culling**: Only process lights that affect visible clusters
- **Direct3D 12**: Low-level API for minimal driver overhead
- **Single-threaded**: Room for multi-threaded command list recording optimization

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