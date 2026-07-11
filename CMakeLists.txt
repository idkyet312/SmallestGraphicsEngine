cmake_minimum_required(VERSION 3.22)
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
    target_compile_options(NvBlast PRIVATE /wd4005 /wd4244 /wd4267)
    set_property(TARGET NvBlast PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Option to choose DX11 or DX12
option(USE_DX12 "Build with DirectX 12 instead of DirectX 11" ON)

# Find imgui package (for ImGui)
find_package(imgui CONFIG)
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

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Header files (for IDE support)
set(HEADER_FILES
    src/CameraDX12.h
    src/ClusteredRenderer.h
    src/ClusteredRendererDX11.h
    src/ClusteredRendererDX12.h
    src/DDGI_DX11.h
    src/DX11Core.h
    src/DX12Core.h
    src/Model.h
    src/ModelDX11.h
    src/Shader.h
    src/ShaderDX11.h
    src/ShaderDX12.h
    src/SceneGraph.h
    src/GLBImporter.h
    src/VisibilityBufferDX12.h
    src/Scene.h
    src/ForwardRenderer.h
    src/ShadowMapDX12.h
    src/IdTechRenderer.h
    src/EngineUI.h
    src/RaytracingDX12.h
    src/MipGenerator.h
    src/MeshShaderDX12.h
    src/TerrainRendererDX12.h
    src/SkyRendererDX12.h
    src/OcclusionDepthDX12.h
    src/DestructionDX12.h
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
        OUTPUT ${CMAKE_BINARY_DIR}/terrain_as.cso
        COMMAND ${DXC_EXECUTABLE} -T as_6_5 -E ASMain
                ${CMAKE_SOURCE_DIR}/shaders/terrain_as.hlsl
                -Fo ${CMAKE_BINARY_DIR}/terrain_as.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/terrain_as.hlsl
        COMMENT "Compiling terrain amplification shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/terrain_ms.cso
        COMMAND ${DXC_EXECUTABLE} -T ms_6_5 -E MSMain
                ${CMAKE_SOURCE_DIR}/shaders/terrain_ms.hlsl
                -Fo ${CMAKE_BINARY_DIR}/terrain_ms.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/terrain_ms.hlsl
        COMMENT "Compiling terrain mesh shader"
    )
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/wire_green_ps.cso
        COMMAND ${DXC_EXECUTABLE} -T ps_6_5 -E main
                ${CMAKE_SOURCE_DIR}/shaders/wire_green_ps.hlsl
                -Fo ${CMAKE_BINARY_DIR}/wire_green_ps.cso
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/wire_green_ps.hlsl
        COMMENT "Compiling wireframe pixel shader"
    )
    add_custom_target(MeshShaderCSO ALL DEPENDS
        ${CMAKE_BINARY_DIR}/mesh_as.cso
        ${CMAKE_BINARY_DIR}/mesh_ms.cso
        ${CMAKE_BINARY_DIR}/mesh_ps.cso
        ${CMAKE_BINARY_DIR}/terrain_as.cso
        ${CMAKE_BINARY_DIR}/terrain_ms.cso
        ${CMAKE_BINARY_DIR}/wire_green_ps.cso)
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
        src/main.cpp
        src/GLBImporter.cpp
        src/FBXImporter.cpp
        src/DestructionDX12.cpp
        ${HEADER_FILES}
        ${SHADER_FILES}
    )
    target_link_libraries(GraphicEngine
        d3d12
        d3dcompiler
        dxgi
        Rpcrt4
        imgui::imgui
        meshoptimizer::meshoptimizer
        unofficial::tinyexr::tinyexr
        assimp::assimp
        box3d
        NvBlast
    )
    target_include_directories(GraphicEngine PRIVATE ${TINYGLTF_INCLUDE_DIRS})
    target_compile_definitions(GraphicEngine PRIVATE USE_DX12=1)
else()
    message(STATUS "Building with DirectX 11")
    add_executable(GraphicEngine WIN32 
        src/main_dx11.cpp
        ${HEADER_FILES}
        ${SHADER_FILES}
    )
    target_link_libraries(GraphicEngine 
        d3d11
        d3dcompiler
        dxgi
        imgui::imgui
    )
    target_include_directories(GraphicEngine PRIVATE ${TINYGLTF_INCLUDE_DIRS})
endif()

# Group files in IDE
source_group("Header Files" FILES ${HEADER_FILES})
source_group("Shader Files" FILES ${SHADER_FILES})

# Set output directory so exe is near shaders folder
set_target_properties(GraphicEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/build"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/build"
)

# Copy shaders to build directory
add_custom_command(TARGET GraphicEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/shaders
    $<TARGET_FILE_DIR:GraphicEngine>/shaders
    COMMENT "Copying shaders to build directory"
)

if(USE_DX12 AND DXC_EXECUTABLE)
    add_custom_command(TARGET GraphicEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:GraphicEngine>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/mesh_as.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_as.cso
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/mesh_ms.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_ms.cso
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/mesh_ps.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/mesh_ps.cso
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/terrain_as.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/terrain_as.cso
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/terrain_ms.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/terrain_ms.cso
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/wire_green_ps.cso $<TARGET_FILE_DIR:GraphicEngine>/shaders/wire_green_ps.cso
    )
    if(DXC_EXECUTABLE)
        add_dependencies(GraphicEngine MeshShaderCSO)
    endif()
endif()

# Also copy to build/shaders for convenience
add_custom_command(TARGET GraphicEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/shaders
    ${CMAKE_SOURCE_DIR}/build/shaders
    COMMENT "Copying shaders to build root"
)

# Copy models to build directory
add_custom_command(TARGET GraphicEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/models
    $<TARGET_FILE_DIR:GraphicEngine>/models
    COMMENT "Copying models to build directory"
)

# Also copy to build/models for convenience
add_custom_command(TARGET GraphicEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/models
    ${CMAKE_SOURCE_DIR}/build/models
    COMMENT "Copying models to build root"
)

