cmake_minimum_required(VERSION 3.10)
project(GraphicEngine)
set(CMAKE_CXX_STANDARD 17)

# Option to choose DX11 or DX12
option(USE_DX12 "Build with DirectX 12 instead of DirectX 11" ON)

# Find imgui package (for ImGui)
find_package(imgui CONFIG)

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
)

# Shader files (for IDE support)
file(GLOB SHADER_FILES "shaders/*.hlsl" "shaders/*.vert" "shaders/*.frag")

# Mark shader files as non-compiled BEFORE adding to target
set_source_files_properties(${SHADER_FILES} PROPERTIES
    HEADER_FILE_ONLY TRUE
    VS_TOOL_OVERRIDE "None"
)

# Create executable based on selected graphics API
if(USE_DX12)
    message(STATUS "Building with DirectX 12")
    add_executable(GraphicEngine WIN32 
        src/main_dx12.cpp
        ${HEADER_FILES}
        ${SHADER_FILES}
    )
    target_link_libraries(GraphicEngine 
        d3d12
        d3dcompiler
        dxgi
        imgui::imgui
    )
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

