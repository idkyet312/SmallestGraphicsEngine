#ifndef MODEL_H
#define MODEL_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
};

class Model {
public:
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    unsigned int VAO, VBO, EBO;
    bool loaded = false;
    
    // Transform relative to camera
    glm::vec3 offset = glm::vec3(0.3f, -0.3f, -0.5f);  // Right, down, forward from camera
    glm::vec3 scale = glm::vec3(0.1f);
    glm::vec3 rotation = glm::vec3(0.0f, 180.0f, 0.0f);  // Rotation in degrees
    glm::vec3 color = glm::vec3(0.3f, 0.3f, 0.3f);
    bool visible = true;
    
    Model() : VAO(0), VBO(0), EBO(0) {}
    
    ~Model() {
        if (loaded) {
            glDeleteVertexArrays(1, &VAO);
            glDeleteBuffers(1, &VBO);
            glDeleteBuffers(1, &EBO);
        }
    }
    
    bool loadOBJ(const std::string& path) {
        std::vector<glm::vec3> temp_positions;
        std::vector<glm::vec3> temp_normals;
        std::vector<unsigned int> positionIndices, normalIndices;
        
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Failed to open OBJ file: " << path << std::endl;
            return false;
        }
        
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string prefix;
            iss >> prefix;
            
            if (prefix == "v") {
                glm::vec3 pos;
                iss >> pos.x >> pos.y >> pos.z;
                temp_positions.push_back(pos);
            }
            else if (prefix == "vn") {
                glm::vec3 normal;
                iss >> normal.x >> normal.y >> normal.z;
                temp_normals.push_back(normal);
            }
            else if (prefix == "f") {
                std::string vertex;
                std::vector<unsigned int> facePositions;
                std::vector<unsigned int> faceNormals;
                
                while (iss >> vertex) {
                    unsigned int posIdx = 0, texIdx = 0, normIdx = 0;
                    
                    // Parse face format: v, v/vt, v/vt/vn, or v//vn
                    size_t firstSlash = vertex.find('/');
                    if (firstSlash == std::string::npos) {
                        // Just position
                        posIdx = std::stoi(vertex);
                    } else {
                        posIdx = std::stoi(vertex.substr(0, firstSlash));
                        size_t secondSlash = vertex.find('/', firstSlash + 1);
                        if (secondSlash == std::string::npos) {
                            // v/vt format
                            texIdx = std::stoi(vertex.substr(firstSlash + 1));
                        } else {
                            // v/vt/vn or v//vn format
                            std::string texPart = vertex.substr(firstSlash + 1, secondSlash - firstSlash - 1);
                            if (!texPart.empty()) {
                                texIdx = std::stoi(texPart);
                            }
                            normIdx = std::stoi(vertex.substr(secondSlash + 1));
                        }
                    }
                    
                    facePositions.push_back(posIdx);
                    faceNormals.push_back(normIdx);
                }
                
                // Triangulate the face (handles quads and n-gons)
                for (size_t i = 1; i + 1 < facePositions.size(); i++) {
                    positionIndices.push_back(facePositions[0]);
                    positionIndices.push_back(facePositions[i]);
                    positionIndices.push_back(facePositions[i + 1]);
                    
                    normalIndices.push_back(faceNormals[0]);
                    normalIndices.push_back(faceNormals[i]);
                    normalIndices.push_back(faceNormals[i + 1]);
                }
            }
        }
        file.close();
        
        // Build vertex array
        vertices.clear();
        indices.clear();
        
        for (size_t i = 0; i < positionIndices.size(); i++) {
            Vertex vertex;
            vertex.Position = temp_positions[positionIndices[i] - 1];  // OBJ indices are 1-based
            
            if (normalIndices[i] > 0 && normalIndices[i] <= temp_normals.size()) {
                vertex.Normal = temp_normals[normalIndices[i] - 1];
            } else {
                vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);  // Default normal
            }
            
            vertices.push_back(vertex);
            indices.push_back(static_cast<unsigned int>(i));
        }
        
        // If no normals were provided, calculate them
        if (temp_normals.empty()) {
            calculateNormals();
        }
        
        setupMesh();
        loaded = true;
        std::cout << "Loaded OBJ: " << path << " (" << vertices.size() << " vertices)" << std::endl;
        return true;
    }
    
    void calculateNormals() {
        // Calculate face normals and assign to vertices
        for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
            glm::vec3 v0 = vertices[i].Position;
            glm::vec3 v1 = vertices[i + 1].Position;
            glm::vec3 v2 = vertices[i + 2].Position;
            
            glm::vec3 normal = glm::normalize(glm::cross(v1 - v0, v2 - v0));
            vertices[i].Normal = normal;
            vertices[i + 1].Normal = normal;
            vertices[i + 2].Normal = normal;
        }
    }
    
    void setupMesh() {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);
        
        glBindVertexArray(VAO);
        
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STATIC_DRAW);
        
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        
        // Position attribute
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
        
        // Normal attribute
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));
        
        glBindVertexArray(0);
    }
    
    glm::mat4 getModelMatrix(const glm::vec3& cameraPos, const glm::vec3& cameraFront, const glm::vec3& cameraUp) {
        // Calculate camera's right vector
        glm::vec3 right = glm::normalize(glm::cross(cameraFront, cameraUp));
        glm::vec3 up = glm::normalize(glm::cross(right, cameraFront));
        
        // Calculate world position based on camera
        glm::vec3 worldPos = cameraPos 
            + right * offset.x 
            + up * offset.y 
            + cameraFront * offset.z;
        
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, worldPos);
        
        // Build a rotation matrix that aligns with camera orientation (yaw and pitch)
        // Create a view-aligned coordinate system
        glm::vec3 front = glm::normalize(cameraFront);
        
        // Calculate yaw (horizontal rotation)
        float yaw = atan2(front.x, front.z);
        
        // Calculate pitch (vertical rotation) 
        float pitch = asin(glm::clamp(front.y, -1.0f, 1.0f));
        
        // Apply camera yaw rotation
        model = glm::rotate(model, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        
        // Apply camera pitch rotation (gun tilts up/down with camera)
        model = glm::rotate(model, -pitch, glm::vec3(1.0f, 0.0f, 0.0f));
        
        // Apply additional user-defined rotation offsets
        model = glm::rotate(model, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        
        model = glm::scale(model, scale);
        
        return model;
    }
    
    void draw() {
        if (!loaded || !visible) return;
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(indices.size()), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
};

#endif

