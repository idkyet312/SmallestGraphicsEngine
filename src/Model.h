#ifndef MODEL_H
#define MODEL_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#define STB_IMAGE_IMPLEMENTATION_MODEL
#include "stb_image.h"

#include <string>
#include <vector>
#include <iostream>
#include <map>

#include "Shader.h"

struct Vertex {
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec2 TexCoords;
    glm::vec3 Tangent;
    glm::vec3 Bitangent;
};

struct Material {
    unsigned int albedoMap;
    unsigned int normalMap;
    unsigned int metallicMap;
    unsigned int roughnessMap;
    unsigned int aoMap;
    
    bool hasAlbedo;
    bool hasNormal;
    bool hasMetallic;
    bool hasRoughness;
    bool hasAO;
    
    glm::vec3 albedoColor;
    float metallicValue;
    float roughnessValue;
    
    Material() : albedoMap(0), normalMap(0), metallicMap(0), roughnessMap(0), aoMap(0),
                 hasAlbedo(false), hasNormal(false), hasMetallic(false), hasRoughness(false), hasAO(false),
                 albedoColor(1.0f), metallicValue(0.0f), roughnessValue(0.5f) {}
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    Material material;
    unsigned int VAO, VBO, EBO;

    Mesh(std::vector<Vertex> vertices, std::vector<unsigned int> indices, Material material) {
        this->vertices = vertices;
        this->indices = indices;
        this->material = material;
        setupMesh();
    }

    void Draw(Shader& shader, unsigned int textureOffset = 3) {
        // Set material properties
        shader.setVec3("objectColor", material.albedoColor);
        shader.setFloat("metallic", material.metallicValue);
        shader.setFloat("roughness", material.roughnessValue);
        
        // Bind textures
        if (material.hasAlbedo) {
            glActiveTexture(GL_TEXTURE0 + textureOffset);
            glBindTexture(GL_TEXTURE_2D, material.albedoMap);
            shader.setInt("material.albedoMap", textureOffset);
        }
        if (material.hasNormal) {
            glActiveTexture(GL_TEXTURE0 + textureOffset + 1);
            glBindTexture(GL_TEXTURE_2D, material.normalMap);
            shader.setInt("material.normalMap", textureOffset + 1);
        }
        if (material.hasMetallic) {
            glActiveTexture(GL_TEXTURE0 + textureOffset + 2);
            glBindTexture(GL_TEXTURE_2D, material.metallicMap);
            shader.setInt("material.metallicMap", textureOffset + 2);
        }
        if (material.hasRoughness) {
            glActiveTexture(GL_TEXTURE0 + textureOffset + 3);
            glBindTexture(GL_TEXTURE_2D, material.roughnessMap);
            shader.setInt("material.roughnessMap", textureOffset + 3);
        }
        if (material.hasAO) {
            glActiveTexture(GL_TEXTURE0 + textureOffset + 4);
            glBindTexture(GL_TEXTURE_2D, material.aoMap);
            shader.setInt("material.aoMap", textureOffset + 4);
        }
        
        shader.setBool("material.hasAlbedoMap", material.hasAlbedo);
        shader.setBool("material.hasNormalMap", material.hasNormal);
        shader.setBool("material.hasMetallicMap", material.hasMetallic);
        shader.setBool("material.hasRoughnessMap", material.hasRoughness);
        shader.setBool("material.hasAOMap", material.hasAO);
        
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
    }
    
    // Simple draw for shadow passes (no materials)
    void DrawSimple() {
        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, indices.size(), GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

private:
    void setupMesh() {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glGenBuffers(1, &EBO);

        glBindVertexArray(VAO);

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), &vertices[0], GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), &indices[0], GL_STATIC_DRAW);

        // Vertex positions
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);

        // Vertex normals
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Normal));

        // Vertex texture coords
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, TexCoords));

        // Vertex tangent
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Tangent));

        // Vertex bitangent
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, Bitangent));

        glBindVertexArray(0);
    }
};

class Model {
public:
    std::vector<Mesh> meshes;
    std::string directory;
    bool loaded;
    glm::vec3 position;
    glm::vec3 rotation;
    glm::vec3 scale;
    std::map<std::string, unsigned int> texturesLoaded;

    Model() : loaded(false), position(0.0f), rotation(0.0f), scale(1.0f) {}

    Model(const std::string& path) : loaded(false), position(0.0f), rotation(0.0f), scale(1.0f) {
        loadModel(path);
    }

    bool loadModel(const std::string& path) {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path, 
            aiProcess_Triangulate | 
            aiProcess_FlipUVs | 
            aiProcess_GenNormals |
            aiProcess_CalcTangentSpace);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
            std::cout << "ERROR::ASSIMP::" << importer.GetErrorString() << std::endl;
            loaded = false;
            return false;
        }

        directory = path.substr(0, path.find_last_of('/'));
        processNode(scene->mRootNode, scene);
        
        loaded = true;
        std::cout << "Model loaded: " << path << " (" << meshes.size() << " meshes)" << std::endl;
        return true;
    }

    void Draw(Shader& shader) {
        if (!loaded) return;
        for (unsigned int i = 0; i < meshes.size(); i++) {
            meshes[i].Draw(shader, 3);
        }
    }
    
    void DrawSimple() {
        if (!loaded) return;
        for (unsigned int i = 0; i < meshes.size(); i++) {
            meshes[i].DrawSimple();
        }
    }

    glm::mat4 getModelMatrix() {
        glm::mat4 model = glm::mat4(1.0f);
        model = glm::translate(model, position);
        model = glm::rotate(model, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, scale);
        return model;
    }

    void clear() {
        meshes.clear();
        loaded = false;
    }
    
    // Load texture for a specific mesh
    bool loadMeshTexture(unsigned int meshIndex, const char* path, int textureType) {
        if (meshIndex >= meshes.size()) return false;
        
        unsigned int texID = loadTextureFromFile(path);
        if (texID == 0) return false;
        
        switch(textureType) {
            case 0: // Albedo
                meshes[meshIndex].material.albedoMap = texID;
                meshes[meshIndex].material.hasAlbedo = true;
                break;
            case 1: // Normal
                meshes[meshIndex].material.normalMap = texID;
                meshes[meshIndex].material.hasNormal = true;
                break;
            case 2: // Metallic
                meshes[meshIndex].material.metallicMap = texID;
                meshes[meshIndex].material.hasMetallic = true;
                break;
            case 3: // Roughness
                meshes[meshIndex].material.roughnessMap = texID;
                meshes[meshIndex].material.hasRoughness = true;
                break;
            case 4: // AO
                meshes[meshIndex].material.aoMap = texID;
                meshes[meshIndex].material.hasAO = true;
                break;
        }
        return true;
    }
    
    // Clear texture for a specific mesh
    void clearMeshTexture(unsigned int meshIndex, int textureType) {
        if (meshIndex >= meshes.size()) return;
        
        switch(textureType) {
            case 0: // Albedo
                meshes[meshIndex].material.hasAlbedo = false;
                meshes[meshIndex].material.albedoMap = 0;
                break;
            case 1: // Normal
                meshes[meshIndex].material.hasNormal = false;
                meshes[meshIndex].material.normalMap = 0;
                break;
            case 2: // Metallic
                meshes[meshIndex].material.hasMetallic = false;
                meshes[meshIndex].material.metallicMap = 0;
                break;
            case 3: // Roughness
                meshes[meshIndex].material.hasRoughness = false;
                meshes[meshIndex].material.roughnessMap = 0;
                break;
            case 4: // AO
                meshes[meshIndex].material.hasAO = false;
                meshes[meshIndex].material.aoMap = 0;
                break;
        }
    }

private:
    void processNode(aiNode* node, const aiScene* scene) {
        // Process all the node's meshes (if any)
        for (unsigned int i = 0; i < node->mNumMeshes; i++) {
            aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            meshes.push_back(processMesh(mesh, scene));
        }
        // Then do the same for each of its children
        for (unsigned int i = 0; i < node->mNumChildren; i++) {
            processNode(node->mChildren[i], scene);
        }
    }
    
    unsigned int loadTextureFromFile(const char* path) {
        unsigned int textureID;
        glGenTextures(1, &textureID);
        
        int width, height, nrComponents;
        unsigned char *data = stbi_load(path, &width, &height, &nrComponents, 0);
        if (data) {
            GLenum format;
            if (nrComponents == 1)
                format = GL_RED;
            else if (nrComponents == 3)
                format = GL_RGB;
            else if (nrComponents == 4)
                format = GL_RGBA;
                
            glBindTexture(GL_TEXTURE_2D, textureID);
            glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
            glGenerateMipmap(GL_TEXTURE_2D);
            
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            
            stbi_image_free(data);
            std::cout << "Loaded texture: " << path << std::endl;
            return textureID;
        } else {
            std::cout << "Failed to load texture: " << path << std::endl;
            stbi_image_free(data);
            return 0;
        }
    }


    Mesh processMesh(aiMesh* mesh, const aiScene* scene) {
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;

        // Process vertices
        for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
            Vertex vertex;
            
            // Position
            vertex.Position = glm::vec3(
                mesh->mVertices[i].x,
                mesh->mVertices[i].y,
                mesh->mVertices[i].z
            );

            // Normals
            if (mesh->HasNormals()) {
                vertex.Normal = glm::vec3(
                    mesh->mNormals[i].x,
                    mesh->mNormals[i].y,
                    mesh->mNormals[i].z
                );
            } else {
                vertex.Normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            // Texture coordinates
            if (mesh->mTextureCoords[0]) {
                vertex.TexCoords = glm::vec2(
                    mesh->mTextureCoords[0][i].x,
                    mesh->mTextureCoords[0][i].y
                );
            } else {
                vertex.TexCoords = glm::vec2(0.0f, 0.0f);
            }

            // Tangent
            if (mesh->HasTangentsAndBitangents()) {
                vertex.Tangent = glm::vec3(
                    mesh->mTangents[i].x,
                    mesh->mTangents[i].y,
                    mesh->mTangents[i].z
                );
                vertex.Bitangent = glm::vec3(
                    mesh->mBitangents[i].x,
                    mesh->mBitangents[i].y,
                    mesh->mBitangents[i].z
                );
            } else {
                vertex.Tangent = glm::vec3(1.0f, 0.0f, 0.0f);
                vertex.Bitangent = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            vertices.push_back(vertex);
        }

        // Process indices
        for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
            aiFace face = mesh->mFaces[i];
            for (unsigned int j = 0; j < face.mNumIndices; j++) {
                indices.push_back(face.mIndices[j]);
            }
        }

        // Process materials
        Material material;
        if (mesh->mMaterialIndex >= 0) {
            aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
            
            // Extract base color/albedo color
            aiColor3D color(1.0f, 1.0f, 1.0f);
            if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
                material.albedoColor = glm::vec3(color.r, color.g, color.b);
            }
            // Also check for base color (glTF 2.0)
            if (mat->Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS) {
                material.albedoColor = glm::vec3(color.r, color.g, color.b);
            }
            
            // Extract metallic value
            float metallic = 0.0f;
            if (mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
                material.metallicValue = metallic;
            }
            
            // Extract roughness value
            float roughness = 0.5f;
            if (mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
                material.roughnessValue = roughness;
            }
            
            // Load albedo/diffuse texture
            if (mat->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_DIFFUSE, 0, &str);
                material.albedoMap = loadTexture(str.C_Str());
                material.hasAlbedo = (material.albedoMap != 0);
            }
            // Also check for base color texture (glTF 2.0)
            if (!material.hasAlbedo && mat->GetTextureCount(aiTextureType_BASE_COLOR) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_BASE_COLOR, 0, &str);
                material.albedoMap = loadTexture(str.C_Str());
                material.hasAlbedo = (material.albedoMap != 0);
            }
            
            // Load normal map
            if (mat->GetTextureCount(aiTextureType_NORMALS) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_NORMALS, 0, &str);
                material.normalMap = loadTexture(str.C_Str());
                material.hasNormal = (material.normalMap != 0);
            } else if (mat->GetTextureCount(aiTextureType_HEIGHT) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_HEIGHT, 0, &str);
                material.normalMap = loadTexture(str.C_Str());
                material.hasNormal = (material.normalMap != 0);
            }
            
            // Load metallic map
            if (mat->GetTextureCount(aiTextureType_METALNESS) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_METALNESS, 0, &str);
                material.metallicMap = loadTexture(str.C_Str());
                material.hasMetallic = (material.metallicMap != 0);
            }
            
            // Load roughness map
            if (mat->GetTextureCount(aiTextureType_DIFFUSE_ROUGHNESS) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_DIFFUSE_ROUGHNESS, 0, &str);
                material.roughnessMap = loadTexture(str.C_Str());
                material.hasRoughness = (material.roughnessMap != 0);
            }
            
            // Load AO map
            if (mat->GetTextureCount(aiTextureType_AMBIENT_OCCLUSION) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_AMBIENT_OCCLUSION, 0, &str);
                material.aoMap = loadTexture(str.C_Str());
                material.hasAO = (material.aoMap != 0);
            } else if (mat->GetTextureCount(aiTextureType_LIGHTMAP) > 0) {
                aiString str;
                mat->GetTexture(aiTextureType_LIGHTMAP, 0, &str);
                material.aoMap = loadTexture(str.C_Str());
                material.hasAO = (material.aoMap != 0);
            }
            
            std::cout << "  Material - Albedo: (" << material.albedoColor.r << ", " 
                      << material.albedoColor.g << ", " << material.albedoColor.b 
                      << "), Metallic: " << material.metallicValue 
                      << ", Roughness: " << material.roughnessValue << std::endl;
        }

        return Mesh(vertices, indices, material);
    }
    
    unsigned int loadTexture(const char* path) {
        std::string filename = std::string(path);
        std::string fullPath = directory + '/' + filename;
        
        // Check if texture was already loaded
        if (texturesLoaded.find(fullPath) != texturesLoaded.end()) {
            return texturesLoaded[fullPath];
        }
        
        unsigned int textureID;
        glGenTextures(1, &textureID);
        
        int width, height, nrComponents;
        unsigned char *data = stbi_load(fullPath.c_str(), &width, &height, &nrComponents, 0);
        if (data) {
            GLenum format;
            if (nrComponents == 1)
                format = GL_RED;
            else if (nrComponents == 3)
                format = GL_RGB;
            else if (nrComponents == 4)
                format = GL_RGBA;
                
            glBindTexture(GL_TEXTURE_2D, textureID);
            glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
            glGenerateMipmap(GL_TEXTURE_2D);
            
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            
            stbi_image_free(data);
            texturesLoaded[fullPath] = textureID;
            std::cout << "Loaded texture: " << fullPath << std::endl;
        } else {
            std::cout << "Failed to load texture: " << fullPath << std::endl;
            stbi_image_free(data);
            return 0;
        }
        
        return textureID;
    }
};

#endif
