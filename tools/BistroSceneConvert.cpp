#include <ufbx.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
std::string Text(ufbx_string s) { return std::string(s.data, s.length); }
ufbx_vec3 Cross(ufbx_vec3 a, ufbx_vec3 b) {
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
std::string Texture(const ufbx_material_map& map) {
    if (!map.texture) return {};
    const auto* t = map.texture;
    if (t->file_textures.count) t = t->file_textures.data[0];
    return Text(t->relative_filename.length ? t->relative_filename : t->filename);
}
json Color(const ufbx_material_map& map, double fallback) {
    if (!map.has_value) return json::array({fallback, fallback, fallback});
    return json::array({map.value_vec3.x, map.value_vec3.y, map.value_vec3.z});
}
struct Vertex { float p[3], n[3], uv[2], t[4]; };
static_assert(sizeof(Vertex) == 48);
void Tangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    std::vector<std::array<double, 3>> tan(vertices.size()), bitan(vertices.size());
    for (size_t i = 0; i < indices.size(); i += 3) {
        const auto& a = vertices[indices[i]];
        const auto& b = vertices[indices[i+1]];
        const auto& c = vertices[indices[i+2]];
        double du1=b.uv[0]-a.uv[0], dv1=b.uv[1]-a.uv[1];
        double du2=c.uv[0]-a.uv[0], dv2=c.uv[1]-a.uv[1];
        double det=du1*dv2-du2*dv1;
        if (std::abs(det)<1e-12) continue;
        for (int k=0;k<3;++k) {
            double e1=b.p[k]-a.p[k], e2=c.p[k]-a.p[k];
            for (int j=0;j<3;++j) {
                tan[indices[i+j]][k]+=(e1*dv2-e2*dv1)/det;
                bitan[indices[i+j]][k]+=(e2*du1-e1*du2)/det;
            }
        }
    }
    for (size_t i=0;i<vertices.size();++i) {
        auto& v=vertices[i];
        ufbx_vec3 n={v.n[0],v.n[1],v.n[2]}, t={tan[i][0],tan[i][1],tan[i][2]};
        double dot=n.x*t.x+n.y*t.y+n.z*t.z;
        t={t.x-n.x*dot,t.y-n.y*dot,t.z-n.z*dot};
        if (t.x*t.x+t.y*t.y+t.z*t.z<1e-20) {
            ufbx_vec3 axis=std::abs(n.y)<0.9 ? ufbx_vec3{0,1,0}:ufbx_vec3{1,0,0};
            t=Cross(axis,n);
        }
        t=ufbx_vec3_normalize(t);
        auto cross=Cross(n,t);
        v.t[0]=float(t.x); v.t[1]=float(t.y); v.t[2]=float(t.z);
        v.t[3]=(cross.x*bitan[i][0]+cross.y*bitan[i][1]+cross.z*bitan[i][2])<0?-1.0f:1.0f;
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: BistroSceneConvert <BistroExterior.fbx> <output-directory>");
        ufbx_load_opts opts{};
        opts.target_axes=ufbx_axes_right_handed_y_up;
        opts.target_unit_meters=1.0;
        opts.generate_missing_normals=true;
        opts.normalize_normals=true;
        ufbx_error error{};
        ufbx_scene* scene=ufbx_load_file(argv[1],&opts,&error);
        if (!scene) throw std::runtime_error(Text(error.description));
        std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)> owner(scene,ufbx_free_scene);
        fs::path output=argv[2]; fs::create_directories(output);
        std::ofstream bin(output/"BistroExterior.bin",std::ios::binary);
        json doc={{"asset",{{"version","2.0"},{"generator","SGE Bistro ufbx converter"}}},
            {"scene",0},{"scenes",json::array({{{"nodes",json::array()}}})},
            {"nodes",json::array()},{"meshes",json::array()},{"materials",json::array()},
            {"bufferViews",json::array()},{"accessors",json::array()}};
        json manifest={{"materials",json::array()}};
        for (auto* m:scene->materials) {
            std::string base=Texture(m->pbr.base_color); if(base.empty()) base=Texture(m->fbx.diffuse_color);
            std::string normal=Texture(m->pbr.normal_map); if(normal.empty()) normal=Texture(m->fbx.normal_map);
            std::string specular=Texture(m->fbx.specular_color);
            std::string emission=Texture(m->pbr.emission_color); if(emission.empty()) emission=Texture(m->fbx.emission_color);
            const auto& emissionMap=m->pbr.emission_color.has_value?m->pbr.emission_color:m->fbx.emission_color;
            double emissionStrength=m->fbx.emission_factor.has_value?m->fbx.emission_factor.value_real:1.0;
            json emissive=Color(emissionMap,emission.empty()?0:1);
            for(auto& value:emissive) value=value.get<double>()*emissionStrength;
            double opacity=m->pbr.opacity.has_value?m->pbr.opacity.value_real:1.0;
            manifest["materials"].push_back({{"name",Text(m->name)},{"baseColor",base},
                {"normal",normal},{"specular",specular},{"emissive",emission},
                {"emissiveFactor",emissive},{"opacity",opacity},
                {"doubleSided",m->features.double_sided.enabled},
                {"transmission",m->features.transmission.enabled},
                {"sourceTextures",json::array()}});
            for(const auto& mt:m->textures) {
                auto* t=mt.texture;
                manifest["materials"].back()["sourceTextures"].push_back({{"property",Text(mt.material_prop)},
                    {"file",Text(t->relative_filename.length?t->relative_filename:t->filename)}});
            }
            doc["materials"].push_back({{"name",Text(m->name)}});
        }
        uint64_t verticesTotal=0,indicesTotal=0;
        std::array<double,3> low={1e30,1e30,1e30},high={-1e30,-1e30,-1e30};
        auto writeView=[&](const void* data,size_t size,int target,int stride=0) {
            uint64_t offset=uint64_t(bin.tellp());
            bin.write(static_cast<const char*>(data),std::streamsize(size));
            int id=int(doc["bufferViews"].size());
            json view={{"buffer",0},{"byteOffset",offset},{"byteLength",size},{"target",target}};
            if(stride) view["byteStride"]=stride;
            doc["bufferViews"].push_back(view); return id;
        };
        for(auto* node:scene->nodes) {
            auto* mesh=node->mesh; if(!mesh) continue;
            std::map<uint32_t,std::vector<Vertex>> groups;
            ufbx_matrix normalMatrix=ufbx_matrix_for_normals(&node->geometry_to_world);
            // The engine is left-handed (XMMatrixLookAtLH) and reads glTF positions
            // as-is, so the right-handed scene is converted here by negating Z.
            // That is itself a mirror: winding flips unless the node already
            // mirrors. Without it the whole street rendered back to front
            // (menu boards read "UNEM", the scooter faced the wrong way).
            bool mirror=ufbx_matrix_determinant(&node->geometry_to_world)>=0;
            std::vector<uint32_t> faceIndices(mesh->max_face_triangles*3);
            for(size_t f=0;f<mesh->faces.count;++f) {
                uint32_t slot=mesh->face_material.count?mesh->face_material.data[f]:0;
                uint32_t mat=slot<node->materials.count?node->materials.data[slot]->typed_id:0;
                auto& out=groups[mat];
                uint32_t triangles=ufbx_triangulate_face(faceIndices.data(),faceIndices.size(),mesh,mesh->faces.data[f]);
                for(uint32_t t=0;t<triangles;++t) {
                    if(mirror) std::swap(faceIndices[t*3+1],faceIndices[t*3+2]);
                    for(uint32_t j=0;j<3;++j) {
                        uint32_t ix=faceIndices[t*3+j]; Vertex v{};
                        auto p=ufbx_transform_position(&node->geometry_to_world,ufbx_get_vertex_vec3(&mesh->vertex_position,ix));
                        auto n=ufbx_vec3_normalize(ufbx_transform_direction(&normalMatrix,ufbx_get_vertex_vec3(&mesh->vertex_normal,ix)));
                        ufbx_vec2 uv=mesh->vertex_uv.exists?ufbx_get_vertex_vec2(&mesh->vertex_uv,ix):ufbx_vec2{};
                        v.p[0]=float(p.x);v.p[1]=float(p.y);v.p[2]=-float(p.z);
                        v.n[0]=float(n.x);v.n[1]=float(n.y);v.n[2]=-float(n.z);
                        v.uv[0]=float(uv.x);v.uv[1]=float(1.0-uv.y);
                        out.push_back(v);
                    }
                }
            }
            json primitives=json::array();
            for(auto& group:groups) {
                auto& vertices=group.second; if(vertices.empty()) continue;
                std::vector<uint32_t> indices(vertices.size());
                ufbx_vertex_stream stream{vertices.data(),vertices.size(),sizeof(Vertex)};
                size_t count=ufbx_generate_indices(&stream,1,indices.data(),indices.size(),nullptr,&error);
                if(!count) throw std::runtime_error("Vertex indexing failed");
                vertices.resize(count); Tangents(vertices,indices);
                std::array<float,3> min={1e30f,1e30f,1e30f},max={-1e30f,-1e30f,-1e30f};
                for(const auto& v:vertices) for(int k=0;k<3;++k) {
                    min[k]=(std::min)(min[k],v.p[k]); max[k]=(std::max)(max[k],v.p[k]);
                    low[k]=(std::min)(low[k],double(v.p[k])); high[k]=(std::max)(high[k],double(v.p[k]));
                }
                int view=writeView(vertices.data(),vertices.size()*sizeof(Vertex),34962,sizeof(Vertex));
                int accessor=int(doc["accessors"].size());
                for(int a=0;a<4;++a) {
                    json entry={{"bufferView",view},{"byteOffset",std::array<int,4>{0,12,24,32}[a]},
                        {"componentType",5126},{"count",count},{"type",std::array<const char*,4>{"VEC3","VEC3","VEC2","VEC4"}[a]}};
                    if(a==0) {entry["min"]=min;entry["max"]=max;} doc["accessors"].push_back(entry);
                }
                int iview=writeView(indices.data(),indices.size()*sizeof(uint32_t),34963);
                int iaccessor=int(doc["accessors"].size());
                doc["accessors"].push_back({{"bufferView",iview},{"componentType",5125},{"count",indices.size()},{"type","SCALAR"}});
                primitives.push_back({{"attributes",{{"POSITION",accessor},{"NORMAL",accessor+1},{"TEXCOORD_0",accessor+2},{"TANGENT",accessor+3}}},
                    {"indices",iaccessor},{"material",group.first},{"mode",4}});
                verticesTotal+=vertices.size();indicesTotal+=indices.size();
            }
            if(primitives.empty()) continue;
            int mid=int(doc["meshes"].size()), nid=int(doc["nodes"].size());
            doc["meshes"].push_back({{"name",Text(node->name)},{"primitives",primitives}});
            doc["nodes"].push_back({{"name",Text(node->name)},{"mesh",mid}});
            doc["scenes"][0]["nodes"].push_back(nid);
        }
        doc["buffers"]=json::array({{{"uri","BistroExterior.bin"},{"byteLength",uint64_t(bin.tellp())}}});
        bin.close(); if(!bin) throw std::runtime_error("Geometry write failed");
        manifest["vertices"]=verticesTotal;manifest["indices"]=indicesTotal;manifest["triangles"]=indicesTotal/3;
        manifest["boundsMin"]=low;manifest["boundsMax"]=high;
        manifest["sourceUnitMeters"]=scene->settings.unit_meters;
        // Authored cameras (Falcor opens the scene on the first one), in the
        // same left-handed space as the geometry.
        manifest["cameras"]=json::array();
        for(auto* camera:scene->cameras) {
            if(!camera->instances.count) continue;
            const ufbx_matrix& m=camera->instances.data[0]->node_to_world;
            static const double axis[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
            auto world=[&](ufbx_coordinate_axis a) {
                ufbx_vec3 d={axis[a][0],axis[a][1],axis[a][2]};
                return ufbx_vec3_normalize(ufbx_transform_direction(&m,d));
            };
            ufbx_vec3 f=world(camera->projection_axes.front),u=world(camera->projection_axes.up);
            manifest["cameras"].push_back({{"name",Text(camera->name)},
                {"position",{m.cols[3].x,m.cols[3].y,-m.cols[3].z}},
                {"forward",{f.x,f.y,-f.z}},{"up",{u.x,u.y,-u.z}},
                {"fovYDegrees",camera->field_of_view_deg.y},
                {"aspect",camera->aspect_ratio}});
        }
        std::ofstream(output/"BistroExterior.gltf")<<doc.dump(2)<<'\n';
        std::ofstream(output/"source-materials.json")<<manifest.dump(2)<<'\n';
        std::cout<<verticesTotal<<" vertices, "<<indicesTotal/3<<" triangles, "<<scene->materials.count<<" materials\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
