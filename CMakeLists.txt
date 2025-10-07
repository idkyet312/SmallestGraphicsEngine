cmake_minimum_required(VERSION 3.10)
project(GraphicEngine)
set(CMAKE_CXX_STANDARD 17)

# Find OpenGL (standard on Windows)
find_package(OpenGL REQUIRED)

# Try to find packages (works with vcpkg or system installations)
find_package(glfw3 CONFIG)
find_package(glm CONFIG)
find_package(glad CONFIG)
find_package(imgui CONFIG)

# If not found, provide helpful message
if(NOT glfw3_FOUND OR NOT glm_FOUND OR NOT glad_FOUND OR NOT imgui_FOUND)
    message(STATUS "========================================")
    message(STATUS "Some dependencies not found via find_package.")
    message(STATUS "Install via vcpkg:")
    message(STATUS "  vcpkg install glfw3 glm glad imgui[glfw-binding,opengl3-binding]")
    message(STATUS "Then configure with:")
    message(STATUS "  cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake")
    message(STATUS "========================================")
    if(NOT glfw3_FOUND)
        message(FATAL_ERROR "glfw3 not found")
    endif()
    if(NOT glm_FOUND)
        message(FATAL_ERROR "glm not found")
    endif()
    if(NOT glad_FOUND)
        message(FATAL_ERROR "glad not found")
    endif()
    if(NOT imgui_FOUND)
        message(FATAL_ERROR "imgui not found")
    endif()
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Create executable
add_executable(GraphicEngine src/main.cpp)

# Link libraries
target_link_libraries(GraphicEngine 
    OpenGL::GL
    glfw
    glad::glad
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
