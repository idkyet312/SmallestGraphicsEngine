cmake_minimum_required(VERSION 3.10)
project(GraphicEngine)
set(CMAKE_CXX_STANDARD 17)

# Find imgui package (for ImGui)
find_package(imgui CONFIG)

# If imgui not found, provide helpful message
if(NOT imgui_FOUND)
    message(STATUS "========================================")
    message(STATUS "imgui not found via find_package.")
    message(STATUS "Install via vcpkg:")
    message(STATUS "  vcpkg install imgui[win32-binding,dx11-binding]")
    message(STATUS "Then configure with:")
    message(STATUS "  cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake")
    message(STATUS "========================================")
    message(FATAL_ERROR "imgui not found")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Create executable (WIN32 for Windows application without console by default, but we create console in code)
add_executable(GraphicEngine WIN32 src/main_dx11.cpp)

# Link libraries for DirectX 11
target_link_libraries(GraphicEngine 
    d3d11
    d3dcompiler
    dxgi
    imgui::imgui
)

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

