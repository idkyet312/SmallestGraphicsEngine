cmake_minimum_required(VERSION 3.26)
project(GraphicEngine)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(box3d
    GIT_REPOSITORY https://github.com/erincatto/box3d.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(box3d)
if(MSVC)
    set_property(TARGET box3d PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Assimp no longer accepts pre-2011 binary FBX. Keep ufbx isolated to the
# offline cooker as a static-geometry fallback for legacy source assets.
FetchContent_Declare(ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG fcc5d6ba444cfd3eb80677dba5e37e493941abe5
)
FetchContent_MakeAvailable(ufbx)
add_library(ufbx STATIC "${ufbx_SOURCE_DIR}/ufbx.c")
target_include_directories(ufbx PUBLIC "${ufbx_SOURCE_DIR}")
target_compile_features(ufbx PUBLIC c_std_99)
if(MSVC)
    set_property(TARGET ufbx PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

file(GLOB NVBLAST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/common/*.cpp"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/lowlevel/*.cpp"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/globals/*.cpp"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/toolkit/*.cpp"
)
add_library(NvBlast STATIC ${NVBLAST_SOURCES})
target_compile_features(NvBlast PUBLIC cxx_std_17)
target_include_directories(NvBlast PUBLIC
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/include/lowlevel"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/include/globals"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/include/toolkit"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/include/shared/NvFoundation"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/common"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/lowlevel"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/globals"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/sdk/toolkit"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/shared/NsFoundation/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/shared/NsFileBuffer/include"
    "${CMAKE_SOURCE_DIR}/thirdparty/blast/source/shared/NvTask/include"
)
if(MSVC)
    target_compile_options(NvBlast PRIVATE /MP /wd4005 /wd4244 /wd4267)
    set_property(TARGET NvBlast PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Vendored stable Recast Navigation. Build only navmesh generation/runtime libs.
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/recastnavigation EXCLUDE_FROM_ALL)

# Option to choose DX11 or DX12
option(USE_DX12 "Build with DirectX 12 instead of DirectX 11" ON)

# Find imgui package (for ImGui)
find_package(imgui CONFIG)
find_package(imguizmo CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)

# If imgui not found, provide helpful message
if(NOT imgui_FOUND)
    message(STATUS "========================================")
    message(STATUS "imgui not found via find_package.")
    message(STATUS "Install via vcpkg:")
    if(USE_DX12)
        message(STATUS "  vcpkg install imgui[win32-binding,dx12-binding]")
    else()
        message(STATUS "  vcpkg install imgui[win32-binding,dx11-binding]")
    endif()
    message(STATUS "Then configure with:")
    message(STATUS "  cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake")
    message(STATUS "========================================")
    message(FATAL_ERROR "imgui not found")
endif()

# Source layout. Headers keep basename includes for now; this interface target
# exposes subsystem folders consistently to the engine, tests, and tools.
set(SGE_SOURCE_ROOT "${CMAKE_SOURCE_DIR}/src")
set(SGE_SOURCE_DIRS
    "${SGE_SOURCE_ROOT}/animation"
    "${SGE_SOURCE_ROOT}/app"
    "${SGE_SOURCE_ROOT}/assets"
    "${SGE_SOURCE_ROOT}/assets/cooked"
    "${SGE_SOURCE_ROOT}/assets/importers"
    "${SGE_SOURCE_ROOT}/assets/prefabs"
    "${SGE_SOURCE_ROOT}/audio"
    "${SGE_SOURCE_ROOT}/core"
    "${SGE_SOURCE_ROOT}/editor"
    "${SGE_SOURCE_ROOT}/gameplay"
    "${SGE_SOURCE_ROOT}/gameplay/player"
    "${SGE_SOURCE_ROOT}/gameplay/weapons"
    "${SGE_SOURCE_ROOT}/level"
    "${SGE_SOURCE_ROOT}/navigation"
    "${SGE_SOURCE_ROOT}/physics"
    "${SGE_SOURCE_ROOT}/render"
    "${SGE_SOURCE_ROOT}/render/dx11"
    "${SGE_SOURCE_ROOT}/render/dx12"
    "${SGE_SOURCE_ROOT}/render/opengl"
    "${SGE_SOURCE_ROOT}/scene"
    "${SGE_SOURCE_ROOT}/world/structures"
    "${SGE_SOURCE_ROOT}/world/terrain"
    "${SGE_SOURCE_ROOT}/world/vegetation"
    "${SGE_SOURCE_ROOT}/world/water"
)
add_library(SGEProjectHeaders INTERFACE)
target_include_directories(SGEProjectHeaders INTERFACE ${SGE_SOURCE_DIRS})

file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
    "${SGE_SOURCE_ROOT}/*.h")
file(GLOB_RECURSE SGE_CPP_FILES CONFIGURE_DEPENDS
    "${SGE_SOURCE_ROOT}/*.cpp")

set(SGE_RUNTIME_SOURCES
    src/assets/AssetRegistry.cpp
    src/assets/AssetWatcher.cpp
    src/assets/cooked/CookedAssetLoader.cpp
    src/assets/importers/FBXImporter.cpp
    src/assets/importers/GLBImporter.cpp
    src/assets/importers/SkinnedFBXImporter.cpp
    src/assets/prefabs/PrefabRegistry.cpp
    src/assets/prefabs/PrefabThumbnailGenerator.cpp
    src/audio/DrMp3.cpp
    src/audio/StbVorbis.cpp
    src/editor/LevelEditor.cpp
    src/gameplay/CombatSystem.cpp
    src/gameplay/weapons/ArmsTextures.cpp
    src/level/LevelDefinition.cpp
    src/navigation/NavigationSystem.cpp
    src/physics/CollisionMesh.cpp
    src/physics/CollisionMeshCache.cpp
    src/physics/DestructionDX12.cpp
    src/physics/SphereMeshCut.cpp
    src/world/terrain/TerrainStampBake.cpp
    src/world/vegetation/PalmMeshCutter.cpp
)

# Find tinygltf
find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")

# Shader files (for IDE support)
file(GLOB SHADER_FILES "shaders/*.hlsl" "shaders/*.vert" "shaders/*.frag")

find_program(DXC_EXECUTABLE dxc)
if(USE_DX12 AND DXC_EXECUTABLE)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/mesh_as.cso
        COMMAND ${DXC_EXECUTABLE} -T as_6_5 -E ASMain
                ${CMAKE_SOURCE_DIR}/shaders/mesh_as.hlsl
                -Fo ${CMAKE_BINARY_DIR}/mesh_as.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/mesh_as.hlsl
        COMMENT "Compiling amplification shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/mesh_ms.cso
        COMMAND ${DXC_EXECUTABLE} -T ms_6_5 -E MSMain
                ${CMAKE_SOURCE_DIR}/shaders/mesh_ms.hlsl
                -Fo ${CMAKE_BINARY_DIR}/mesh_ms.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/mesh_ms.hlsl
        COMMENT "Compiling mesh shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/mesh_ps.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main
                ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/mesh_ps.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
        COMMENT "Compiling mesh-path pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/mesh_ps_hdr.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main -D SGE_HDR_TARGET=1
                ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/mesh_ps_hdr.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
        COMMENT "Compiling HDR mesh-path pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/mesh_ps_motion.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main
                -D SGE_EXTENSION_MOTION=1
                -D SGE_HDR_TARGET=1
                ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/mesh_ps_motion.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
        COMMENT "Compiling extension-motion mesh-path HDR pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/terrain_as.cso
        COMMAND ${DXC_EXECUTABLE} -T as_6_5 -E ASMain
                ${CMAKE_SOURCE_DIR}/shaders/terrain_as.hlsl
                -Fo ${CMAKE_BINARY_DIR}/terrain_as.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/terrain_as.hlsl
                ${CMAKE_SOURCE_DIR}/shaders/terrain_height.hlsli
        COMMENT "Compiling terrain amplification shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/terrain_ms.cso
        COMMAND ${DXC_EXECUTABLE} -T ms_6_5 -E MSMain
                ${CMAKE_SOURCE_DIR}/shaders/terrain_ms.hlsl
                -Fo ${CMAKE_BINARY_DIR}/terrain_ms.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/terrain_ms.hlsl
                ${CMAKE_SOURCE_DIR}/shaders/terrain_height.hlsli
        COMMENT "Compiling terrain mesh shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/terrain_ps.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main -D SGE_TERRAIN_PBR=1
                -I ${CMAKE_SOURCE_DIR}/shaders
                ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/terrain_ps.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
                ${CMAKE_SOURCE_DIR}/shaders/terrain_pbr.hlsli
        COMMENT "Compiling triplanar terrain PBR pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/terrain_ps_hdr.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main -D SGE_TERRAIN_PBR=1
                -D SGE_HDR_TARGET=1 -I ${CMAKE_SOURCE_DIR}/shaders
                ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/terrain_ps_hdr.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/clustered_dx12_ps.hlsl
                ${CMAKE_SOURCE_DIR}/shaders/terrain_pbr.hlsli
        COMMENT "Compiling HDR triplanar terrain PBR pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/terrain_visibility_ps.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main
                ${CMAKE_SOURCE_DIR}/shaders/terrain_visibility_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/terrain_visibility_ps.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/terrain_visibility_ps.hlsl
        COMMENT "Compiling terrain visibility-ID pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/wire_green_ps.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main
                ${CMAKE_SOURCE_DIR}/shaders/wire_green_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/wire_green_ps.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/wire_green_ps.hlsl
        COMMENT "Compiling wireframe pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/water_vs.cso
        COMMAND ${DXC_EXECUTABLE} -T vs_6_0 -E VSMain
                ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
                -Fo ${CMAKE_BINARY_DIR}/water_vs.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
        COMMENT "Compiling tropical water vertex shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/water_ps_hdr.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain
                -D WATER_HDR_TARGET=1 -D WATER_MOTION_OUTPUT=1
                ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
                -Fo ${CMAKE_BINARY_DIR}/water_ps_hdr.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
        COMMENT "Compiling HDR tropical water pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/water_ps_ldr.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain
                ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
                -Fo ${CMAKE_BINARY_DIR}/water_ps_ldr.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
        COMMENT "Compiling forward tropical water pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/water_ps_ldr_msaa.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_0 -E PSMain
                -D WATER_DEPTH_MSAA=1
                ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
                -Fo ${CMAKE_BINARY_DIR}/water_ps_ldr_msaa.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/water_dx12.hlsl
        COMMENT "Compiling MSAA-depth tropical water pixel shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/ultra_water_spectrum_cs.cso
        COMMAND ${DXC_EXECUTABLE} -T cs_6_5 -E SpectrumCS
                ${CMAKE_SOURCE_DIR}/shaders/ultra_water_cs.hlsl
                -Fo ${CMAKE_BINARY_DIR}/ultra_water_spectrum_cs.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/ultra_water_cs.hlsl
        COMMENT "Compiling Ultra water spectral simulation"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/ultra_water_coast_cs.cso
        COMMAND ${DXC_EXECUTABLE} -T cs_6_5 -E CoastCS
                ${CMAKE_SOURCE_DIR}/shaders/ultra_water_cs.hlsl
                -Fo ${CMAKE_BINARY_DIR}/ultra_water_coast_cs.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/ultra_water_cs.hlsl
        COMMENT "Compiling Ultra water coastal simulation"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/ultra_water_height_query_cs.cso
        COMMAND ${DXC_EXECUTABLE} -T cs_6_5 -E HeightQueryCS
                ${CMAKE_SOURCE_DIR}/shaders/ultra_water_cs.hlsl
                -Fo ${CMAKE_BINARY_DIR}/ultra_water_height_query_cs.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/ultra_water_cs.hlsl
        COMMENT "Compiling Ultra water height queries"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/dxr_ddgi.cso
        COMMAND ${DXC_EXECUTABLE} -T lib_6_3
                ${CMAKE_SOURCE_DIR}/shaders/dxr_ddgi.hlsl
                -Fo ${CMAKE_BINARY_DIR}/dxr_ddgi.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/dxr_ddgi.hlsl
        COMMENT "Compiling DXR sparse DDGI library"
    )
    add_custom_target(MeshShaderCSO ALL DEPENDS
        ${CMAKE_BINARY_DIR}/mesh_as.cso
        ${CMAKE_BINARY_DIR}/mesh_ms.cso
        ${CMAKE_BINARY_DIR}/mesh_ps.cso
        ${CMAKE_BINARY_DIR}/mesh_ps_hdr.cso
        ${CMAKE_BINARY_DIR}/mesh_ps_motion.cso
        ${CMAKE_BINARY_DIR}/terrain_as.cso
        ${CMAKE_BINARY_DIR}/terrain_ms.cso
        ${CMAKE_BINARY_DIR}/terrain_ps.cso
        ${CMAKE_BINARY_DIR}/terrain_ps_hdr.cso
        ${CMAKE_BINARY_DIR}/terrain_visibility_ps.cso
        ${CMAKE_BINARY_DIR}/wire_green_ps.cso
        ${CMAKE_BINARY_DIR}/water_vs.cso
        ${CMAKE_BINARY_DIR}/water_ps_hdr.cso
        ${CMAKE_BINARY_DIR}/water_ps_ldr.cso
        ${CMAKE_BINARY_DIR}/water_ps_ldr_msaa.cso
        ${CMAKE_BINARY_DIR}/ultra_water_spectrum_cs.cso
        ${CMAKE_BINARY_DIR}/ultra_water_coast_cs.cso
        ${CMAKE_BINARY_DIR}/ultra_water_height_query_cs.cso
        ${CMAKE_BINARY_DIR}/dxr_ddgi.cso)
else()
    message(STATUS "DXC not found; mesh shader path will use raster fallback")
endif()

# Mark shader files as non-compiled BEFORE adding to target
set_source_files_properties(${SHADER_FILES} PROPERTIES
    HEADER_FILE_ONLY TRUE
    VS_TOOL_OVERRIDE "None"
)

# Create executable based on selected graphics API
if(USE_DX12)
    message(STATUS "Building with DirectX 12")
    add_executable(GraphicEngine WIN32
        src/app/main.cpp
        ${SGE_RUNTIME_SOURCES}
        ${HEADER_FILES}
        ${SHADER_FILES}
    )
    target_link_libraries(GraphicEngine
        SGEProjectHeaders
        d3d12
        d3dcompiler
        dxgi
        Rpcrt4
        xaudio2
        imgui::imgui
        imguizmo::imguizmo
        nlohmann_json::nlohmann_json
        meshoptimizer::meshoptimizer
        unofficial::tinyexr::tinyexr
        assimp::assimp
        box3d
        RecastNavigation::Recast
        RecastNavigation::Detour
        NvBlast
    )
    target_include_directories(GraphicEngine PRIVATE ${TINYGLTF_INCLUDE_DIRS})
    target_compile_definitions(GraphicEngine PRIVATE USE_DX12=1)
else()
    message(STATUS "Building with DirectX 11")
    add_executable(GraphicEngine WIN32 
        src/app/main_dx11.cpp
        ${HEADER_FILES}
        ${SHADER_FILES}
    )
    target_link_libraries(GraphicEngine 
        SGEProjectHeaders
        d3d11
        d3dcompiler
        dxgi
        imgui::imgui
    )
    target_include_directories(GraphicEngine PRIVATE ${TINYGLTF_INCLUDE_DIRS})
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(LevelDefinitionTests
        tests/LevelDefinitionTests.cpp
        tests/GLBImporterPixelStub.cpp
        tests/StbImageWriteImpl.cpp
        src/level/LevelDefinition.cpp)
    target_link_libraries(LevelDefinitionTests PRIVATE
        SGEProjectHeaders nlohmann_json::nlohmann_json)
    add_test(NAME LevelDefinitionTests COMMAND LevelDefinitionTests)

    # Header-only and free of any device, so they need nothing but the headers.
    add_executable(GameSettingsTests tests/GameSettingsTests.cpp)
    target_link_libraries(GameSettingsTests PRIVATE SGEProjectHeaders)
    add_test(NAME GameSettingsTests COMMAND GameSettingsTests)

    add_executable(MoneySystemTests tests/MoneySystemTests.cpp)
    target_link_libraries(MoneySystemTests PRIVATE SGEProjectHeaders)
    add_test(NAME MoneySystemTests COMMAND MoneySystemTests)

    add_executable(UISearchFilterTests tests/UISearchFilterTests.cpp)
    target_link_libraries(UISearchFilterTests PRIVATE SGEProjectHeaders)
    add_test(NAME UISearchFilterTests COMMAND UISearchFilterTests)

    add_executable(BulletPenetrationTests tests/BulletPenetrationTests.cpp)
    target_link_libraries(BulletPenetrationTests PRIVATE SGEProjectHeaders)
    add_test(NAME BulletPenetrationTests COMMAND BulletPenetrationTests)

    add_executable(RuntimeWorldTests
        tests/RuntimeWorldTests.cpp
        tests/GLBImporterPixelStub.cpp
        tests/StbImageWriteImpl.cpp
        src/level/LevelDefinition.cpp)
    target_link_libraries(RuntimeWorldTests PRIVATE
        SGEProjectHeaders nlohmann_json::nlohmann_json)
    add_test(NAME RuntimeWorldTests COMMAND RuntimeWorldTests)

    add_executable(GameArchitectureTests
        tests/GameArchitectureTests.cpp
        tests/GLBImporterPixelStub.cpp
        tests/StbImageWriteImpl.cpp
        src/gameplay/CombatSystem.cpp
        src/level/LevelDefinition.cpp)
    target_link_libraries(GameArchitectureTests PRIVATE
        SGEProjectHeaders nlohmann_json::nlohmann_json)
    add_test(NAME GameArchitectureTests COMMAND GameArchitectureTests)

    # Backs the enemy scatter test mode. Links Recast/Detour directly because it
    # builds a real navmesh rather than stubbing one -- the guarantee under test
    # is Detour's, so a fake mesh would verify nothing.
    add_executable(NavigationRandomPointTests
        tests/NavigationRandomPointTests.cpp
        src/navigation/NavigationSystem.cpp)
    target_link_libraries(NavigationRandomPointTests PRIVATE
        SGEProjectHeaders
        RecastNavigation::Recast
        RecastNavigation::Detour)
    add_test(NAME NavigationRandomPointTests COMMAND NavigationRandomPointTests)

    add_executable(PrefabRegistryTests
        tests/PrefabRegistryTests.cpp
        src/assets/prefabs/PrefabRegistry.cpp)
    # Lets the test reach the real Content/ tree to validate shipped prefabs,
    # independent of the working directory ctest happens to run it from.
    target_compile_definitions(PrefabRegistryTests PRIVATE
        SGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(PrefabRegistryTests PRIVATE
        SGEProjectHeaders nlohmann_json::nlohmann_json assimp::assimp)
    add_test(NAME PrefabRegistryTests COMMAND PrefabRegistryTests)

    add_executable(AssetRegistryTests
        tests/AssetRegistryTests.cpp
        src/assets/AssetRegistry.cpp)
    target_include_directories(AssetRegistryTests PRIVATE
        ${TINYGLTF_INCLUDE_DIRS})
    target_link_libraries(AssetRegistryTests PRIVATE
        SGEProjectHeaders nlohmann_json::nlohmann_json assimp::assimp)
    add_test(NAME AssetRegistryTests COMMAND AssetRegistryTests)

    add_executable(PrefabThumbnailTests
        tests/PrefabThumbnailTests.cpp
        tests/StbImageWriteImpl.cpp
        src/assets/prefabs/PrefabThumbnailGenerator.cpp)
    target_include_directories(PrefabThumbnailTests PRIVATE
        ${TINYGLTF_INCLUDE_DIRS})
    target_link_libraries(PrefabThumbnailTests PRIVATE
        SGEProjectHeaders assimp::assimp)
    add_test(NAME PrefabThumbnailTests COMMAND PrefabThumbnailTests)

    add_executable(AssetWatcherTests
        tests/AssetWatcherTests.cpp
        src/assets/AssetWatcher.cpp)
    target_link_libraries(AssetWatcherTests PRIVATE SGEProjectHeaders)
    add_test(NAME AssetWatcherTests COMMAND AssetWatcherTests)

    # Diagnostic for the FPS arms asset: prints raw per-mesh bounds from Assimp.
    add_executable(InspectArms tests/InspectArms.cpp)
    target_link_libraries(InspectArms PRIVATE assimp::assimp)

    add_executable(DXRProbeLayoutTests tests/DXRProbeLayoutTests.cpp)
    target_link_libraries(DXRProbeLayoutTests PRIVATE
        SGEProjectHeaders nlohmann_json::nlohmann_json)
    add_test(NAME DXRProbeLayoutTests COMMAND DXRProbeLayoutTests)

    # Perf benchmark for the CPU clustered cull (Phase 2 decision input). Not
    # registered with ctest -- it prints timings, it does not assert.
    add_executable(ClusteredCullBench tests/ClusteredCullBench.cpp)
    target_link_libraries(ClusteredCullBench PRIVATE SGEProjectHeaders)

    add_executable(CookedAssetFormatTests tests/CookedAssetFormatTests.cpp)
    target_link_libraries(CookedAssetFormatTests PRIVATE SGEProjectHeaders)
    add_test(NAME CookedAssetFormatTests COMMAND CookedAssetFormatTests)

    # Pins the DXBC of the default (FXC, non-bindless) visibility resolve, so a
    # refactor behind a feature guard cannot silently reshape the shader every
    # pixel goes through. Compiles HLSL only -- no device needed.
    add_executable(ResolveShaderCanaryTests tests/ResolveShaderCanaryTests.cpp)
    target_link_libraries(ResolveShaderCanaryTests PRIVATE
        SGEProjectHeaders d3dcompiler)
    target_compile_definitions(ResolveShaderCanaryTests PRIVATE
        SGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ResolveShaderCanaryTests COMMAND ResolveShaderCanaryTests)

    add_executable(SniperScopeProjectionTests tests/SniperScopeProjectionTests.cpp)
    target_link_libraries(SniperScopeProjectionTests PRIVATE
        SGEProjectHeaders d3d11 d3dcompiler)
    target_compile_definitions(SniperScopeProjectionTests PRIVATE
        SGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME SniperScopeProjectionTests COMMAND SniperScopeProjectionTests)

    # Pure CPU test for the bindless descriptor index allocator. Links no D3D
    # libraries on purpose -- BindlessHeapDX12.h keeps the range arithmetic
    # free of device calls so it can be tested without a GPU.
    add_executable(BindlessHeapTests tests/BindlessHeapTests.cpp)
    target_link_libraries(BindlessHeapTests PRIVATE SGEProjectHeaders)
    add_test(NAME BindlessHeapTests COMMAND BindlessHeapTests)

    add_executable(VisibilityGeometryPoolTests tests/VisibilityGeometryPoolTests.cpp)
    target_link_libraries(VisibilityGeometryPoolTests PRIVATE SGEProjectHeaders)
    add_test(NAME VisibilityGeometryPoolTests COMMAND VisibilityGeometryPoolTests)

    add_executable(TextureUploadArenaTests tests/TextureUploadArenaTests.cpp)
    target_link_libraries(TextureUploadArenaTests PRIVATE SGEProjectHeaders d3d12)
    add_test(NAME TextureUploadArenaTests COMMAND TextureUploadArenaTests)

    add_executable(UltraWaterSimulationTests tests/UltraWaterSimulationTests.cpp)
    target_link_libraries(UltraWaterSimulationTests PRIVATE SGEProjectHeaders)
    add_test(NAME UltraWaterSimulationTests COMMAND UltraWaterSimulationTests)

    # Per-triangle collision BVH. Pure CPU: CollisionMesh takes raw float
    # arrays and never sees the scene graph or a device, so the build and the
    # traversal are testable without a GPU or any game content.
    add_executable(CollisionMeshTests
        tests/CollisionMeshTests.cpp
        src/physics/CollisionMesh.cpp)
    target_link_libraries(CollisionMeshTests PRIVATE SGEProjectHeaders)
    add_test(NAME CollisionMeshTests COMMAND CollisionMeshTests)

    # Runtime fracture planes. Pure geometry: the cut math is split into its own
    # header precisely so the fragment partition can be checked without a device,
    # a Blast framework or any level content.
    add_executable(RuntimeCutPlaneTests tests/RuntimeCutPlaneTests.cpp)
    target_link_libraries(RuntimeCutPlaneTests PRIVATE SGEProjectHeaders)
    add_test(NAME RuntimeCutPlaneTests COMMAND RuntimeCutPlaneTests)

    # Runtime sphere boolean, split out for the same reason: a grenade blowing a
    # hole in a wall is arithmetic, so the crater can be checked without a
    # device or any game content.
    add_executable(SphereMeshCutTests
        tests/SphereMeshCutTests.cpp src/physics/SphereMeshCut.cpp)
    target_link_libraries(SphereMeshCutTests PRIVATE SGEProjectHeaders)
    add_test(NAME SphereMeshCutTests COMMAND SphereMeshCutTests)

    add_executable(RagdollAssetTests tests/RagdollAssetTests.cpp)
    target_link_libraries(RagdollAssetTests PRIVATE SGEProjectHeaders)
    target_compile_definitions(RagdollAssetTests PRIVATE
        SGE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME RagdollAssetTests COMMAND RagdollAssetTests)
endif()

if(MSVC)
    # Compile independent translation units concurrently. Especially useful
    # after header changes that invalidate main.cpp and renderer sources.
    # /bigobj keeps Debug builds viable while the gameplay monolith is moved
    # incrementally behind RuntimeWorld.
    target_compile_options(GraphicEngine PRIVATE /MP /bigobj)
endif()

# Offline source-asset ingestion. Runtime code prefers resulting versioned
# .sgeasset files; source import remains an editor/development fallback.
add_executable(AssetCooker
    tools/AssetCooker.cpp
    src/assets/cooked/CookedAssetFormat.h)
target_link_libraries(AssetCooker PRIVATE
    SGEProjectHeaders
    assimp::assimp
    meshoptimizer::meshoptimizer
    ufbx)
if(MSVC)
    target_compile_options(AssetCooker PRIVATE /MP /bigobj)
endif()

add_custom_target(CookAssets
    COMMAND $<TARGET_FILE:AssetCooker>
        --all "${CMAKE_SOURCE_DIR}/Content"
        --out "${CMAKE_SOURCE_DIR}/Content/Cooked"
    DEPENDS AssetCooker
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Cooking FBX/GLB/GLTF assets into runtime .sgeasset blobs")

# Mirror physical subsystem folders in Visual Studio.
source_group(TREE "${SGE_SOURCE_ROOT}" PREFIX "Source"
    FILES ${HEADER_FILES} ${SGE_CPP_FILES})
source_group(TREE "${CMAKE_SOURCE_DIR}/shaders" PREFIX "Shaders"
    FILES ${SHADER_FILES})

# Keep each CMake build tree self-contained. The default tree still produces
# build/GraphicEngine.exe; alternate trees produce their own executable instead
# of overwriting or being locked by the default build.
set_target_properties(GraphicEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/MinSizeRel"
)

# Copy shaders to build directory
add_custom_command(TARGET GraphicEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
    ${CMAKE_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:GraphicEngine>/shaders
    # scripts/ is gitignored and may not exist at all in a fresh clone or a
    # git worktree. copy_directory_if_different fails hard on a missing
    # source, which broke the build in a worktree while the already-built exe
    # kept running -- so make the directory first and copy what is there.
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/scripts
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
    ${CMAKE_SOURCE_DIR}/scripts
    $<TARGET_FILE_DIR:GraphicEngine>/scripts
    COMMENT "Copying shaders and data-driven content to build directory"
)

if(USE_DX12 AND DXC_EXECUTABLE)
    add_custom_command(TARGET GraphicEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:GraphicEngine>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/mesh_as.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_as.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/mesh_ms.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_ms.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/mesh_ps.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_ps.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/mesh_ps_hdr.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_ps_hdr.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/mesh_ps_motion.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_ps_motion.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/terrain_as.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/terrain_as.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/terrain_ms.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/terrain_ms.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/terrain_ps.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/terrain_ps.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/terrain_ps_hdr.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/terrain_ps_hdr.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/terrain_visibility_ps.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/terrain_visibility_ps.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/wire_green_ps.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/wire_green_ps.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/water_vs.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/water_vs.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/water_ps_hdr.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/water_ps_hdr.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/water_ps_ldr.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/water_ps_ldr.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/water_ps_ldr_msaa.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/water_ps_ldr_msaa.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/ultra_water_spectrum_cs.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/ultra_water_spectrum_cs.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/ultra_water_coast_cs.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/ultra_water_coast_cs.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/ultra_water_height_query_cs.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/ultra_water_height_query_cs.cso
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/dxr_ddgi.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/dxr_ddgi.cso
    )
    if(DXC_EXECUTABLE)
        add_dependencies(GraphicEngine MeshShaderCSO)
    endif()
endif()

# Content (Unreal-style asset root: Models, Audio, Textures, Levels, Prefabs)
# is several GiB. Compare a small source metadata manifest on every build so a
# changed asset does not force CMake to read the entire source and destination
# trees byte-for-byte.
set(CONTENT_SYNC_MANIFEST "${CMAKE_BINARY_DIR}/.content-manifest")
add_custom_target(RuntimeContent
    COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/Content"
            "-DDESTINATION_DIR=${CMAKE_BINARY_DIR}/Content"
            "-DMANIFEST_PATH=${CONTENT_SYNC_MANIFEST}"
            -P "${CMAKE_SOURCE_DIR}/cmake/SyncRuntimeContent.cmake"
    BYPRODUCTS "${CONTENT_SYNC_MANIFEST}"
    COMMENT "Syncing changed runtime content"
    VERBATIM
)
add_dependencies(GraphicEngine RuntimeContent)
