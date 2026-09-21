#include "DirectionalLocomotion.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <fstream>

using namespace DirectX;
static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
static XMFLOAT4X4 Matrix(const aiMatrix4x4& m) {
    return {m.a1,m.b1,m.c1,m.d1,m.a2,m.b2,m.c2,m.d2,
            m.a3,m.b3,m.c3,m.d3,m.a4,m.b4,m.c4,m.d4};
}
static void Bones(const aiNode* node, int parent, Skeleton& s) {
    const int id = static_cast<int>(s.names.size());
    s.names.push_back(node->mName.C_Str()); s.index[s.names.back()] = id;
    s.parent.push_back(parent); s.localBind.push_back(Matrix(node->mTransformation));
    XMFLOAT4X4 identity; XMStoreFloat4x4(&identity, XMMatrixIdentity()); s.offset.push_back(identity);
    for (unsigned i=0; i<node->mNumChildren; ++i) Bones(node->mChildren[i], id, s);
}

// One AnimStack resolved onto `skeleton` by bone name, the way the importer
// builds a clip. Shared by both cases below so the adopted cycles are read
// exactly as the baked ones are.
static bool ReadClip(const aiScene* scene, const Skeleton& skeleton,
                     const std::string& name, AnimationClip& clip) {
    if (!scene || !scene->mNumAnimations) return false;
    const aiAnimation& a=*scene->mAnimations[0];
    const double rate=a.mTicksPerSecond ? a.mTicksPerSecond : 30;
    clip.name=name; clip.duration=static_cast<float>(a.mDuration/rate);
    for (unsigned c=0;c<a.mNumChannels;++c) {
        const auto& channel=*a.mChannels[c];
        const int bone=skeleton.Find(channel.mNodeName.C_Str()); if (bone<0) continue;
        BoneTrack track; track.bone=bone;
        for (unsigned k=0;k<channel.mNumPositionKeys;++k) {
            const auto& key=channel.mPositionKeys[k];
            track.positions.push_back({float(key.mTime/rate),{key.mValue.x,key.mValue.y,key.mValue.z}});
        }
        for (unsigned k=0;k<channel.mNumRotationKeys;++k) {
            const auto& key=channel.mRotationKeys[k];
            track.rotations.push_back({float(key.mTime/rate),{key.mValue.x,key.mValue.y,key.mValue.z,key.mValue.w}});
        }
        for (unsigned k=0;k<channel.mNumScalingKeys;++k) {
            const auto& key=channel.mScalingKeys[k];
            track.scales.push_back({float(key.mTime/rate),{key.mValue.x,key.mValue.y,key.mValue.z}});
        }
        clip.tracks.push_back(std::move(track));
    }
    return !clip.tracks.empty();
}

int main(int argc, char** argv) {
    for (int x=-20; x<=20; ++x) for (int y=-20; y<=20; ++y) {
        auto w = LocomotionBlendSpace::Weights(x*0.2f,y*0.2f,1.8f);
        float sum=0; for (float v:w) { Check(v>=0 && std::isfinite(v), "Invalid blend weight"); sum+=v; }
        Check(std::abs(sum-1)<1e-5f,"Weights must sum to one");
    }
    Check(LocomotionBlendSpace::Weights(0,1.8f,1.8f)[1] > .999f,"Forward selects forward walk");
    Check(LocomotionBlendSpace::Weights(0,-1.8f,1.8f)[2] > .999f,"Backward selects backward walk");
    Check(LocomotionBlendSpace::Weights(-1.8f,0,1.8f)[3] > .999f,"Left selects left walk");
    Check(LocomotionBlendSpace::Weights(2.97f,0,1.8f)[8] > .999f,"Fast right selects right run");
    Check(argc>1,"Pass repository root for real bandit asset tests");
    const std::string dir=std::string(argv[1])+"/Content/Models/MilitaryMercenaryBandit/";
    Assimp::Importer mesh;
    const aiScene* scene=mesh.ReadFile(dir+"SK_Bandit.FBX",0);
    Check(scene && scene->mRootNode,"Load bandit skeleton");
    Skeleton skeleton; Bones(scene->mRootNode,-1,skeleton);
    XMStoreFloat4x4(&skeleton.globalInverse,XMMatrixInverse(nullptr,XMLoadFloat4x4(&skeleton.localBind[0])));
    std::vector<AnimationClip> clips;
    for (const char* name : {"Idle","Walk","Run"}) {
        Assimp::Importer importer;
        scene=importer.ReadFile(dir+"Animations/Demo/ThirdPerson"+name+".FBX",0);
        Check(scene && scene->mNumAnimations,"Load source gait");
        const aiAnimation& a=*scene->mAnimations[0];
        const double rate=a.mTicksPerSecond ? a.mTicksPerSecond : 30;
        AnimationClip clip; clip.name=std::string("ThirdPerson")+name; clip.duration=static_cast<float>(a.mDuration/rate);
        for (unsigned c=0;c<a.mNumChannels;++c) {
            const auto& channel=*a.mChannels[c];
            const int bone=skeleton.Find(channel.mNodeName.C_Str()); if (bone<0) continue;
            BoneTrack track; track.bone=bone;
            for (unsigned k=0;k<channel.mNumPositionKeys;++k) {
                const auto& key=channel.mPositionKeys[k];
                track.positions.push_back({float(key.mTime/rate),{key.mValue.x,key.mValue.y,key.mValue.z}});
            }
            for (unsigned k=0;k<channel.mNumRotationKeys;++k) {
                const auto& key=channel.mRotationKeys[k];
                track.rotations.push_back({float(key.mTime/rate),{key.mValue.x,key.mValue.y,key.mValue.z,key.mValue.w}});
            }
            for (unsigned k=0;k<channel.mNumScalingKeys;++k) {
                const auto& key=channel.mScalingKeys[k];
                track.scales.push_back({float(key.mTime/rate),{key.mValue.x,key.mValue.y,key.mValue.z}});
            }
            clip.tracks.push_back(std::move(track));
        }
        clips.push_back(std::move(clip));
    }
    Check(DirectionalLocomotion::Bake(skeleton,clips),"Bake directional clips on the real skeleton");
    Check(clips.size()==11,"Eight new directional clips");
    std::ofstream preview;
    if (argc>2) {
        preview.open(argv[2]);
        preview << "clip,frame,bone,parent,x,y,z\n";
    }
    for (size_t i=3;i<clips.size();++i) {
        AnimationInstance anim; anim.Play(&clips[i]);
        std::vector<XMFLOAT4X4> globals;
        for (int sample=0;sample<=32;++sample) {
            anim.time=anim.clip->duration*sample/32; anim.ComputeGlobalMatrices(skeleton,globals);
            // Only the IK-baked sideways slots. Named rather than indexed so
            // the check follows the clip if the list is reordered, and scoped
            // to this bake because an authored running strafe really does
            // cross its trailing leg over -- see the adoption case below.
            const std::string& baked=clips[i].name;
            if (baked=="WalkLeft"||baked=="WalkRight"||baked=="RunLeft"||baked=="RunRight")
                Check(globals[skeleton.Find("foot_l")]._41 > globals[skeleton.Find("foot_r")]._41,
                      "Strafe feet must not cross");
            AnimationInstance reference;
            reference.Play(&clips[i<7 ? 3 : 7]); reference.time=reference.clip->duration*sample/32;
            std::vector<XMFLOAT4X4> referenceGlobals;
            reference.ComputeGlobalMatrices(skeleton,referenceGlobals);
            for (size_t b=0;b<globals.size();++b) {
                for (const auto& row:globals[b].m) for (float v:row) Check(std::isfinite(v),"Finite pose");
                if (preview) preview<<clips[i].name<<','<<sample<<','<<skeleton.names[b]<<','
                    <<(skeleton.parent[b]<0 ? "none" : skeleton.names[skeleton.parent[b]])<<','
                    <<globals[b]._41<<','<<globals[b]._42<<','<<globals[b]._43<<'\n';
                const int p=skeleton.parent[b]; if (p<0) continue;
                const float length=XMVectorGetX(XMVector3Length(XMLoadFloat4x4(&globals[b]).r[3]-XMLoadFloat4x4(&globals[p]).r[3]));
                const float expected=XMVectorGetX(XMVector3Length(XMLoadFloat4x4(&referenceGlobals[b]).r[3]-XMLoadFloat4x4(&referenceGlobals[p]).r[3]));
                Check(std::abs(length-expected)<.02f,"Directional IK preserves bone lengths");
            }
        }
        for (const auto& track:clips[i].tracks) {
            const auto& a=track.rotations.front().value; const auto& b=track.rotations.back().value;
            Check(std::abs(std::abs(XMVectorGetX(XMVector4Dot(XMLoadFloat4(&a),XMLoadFloat4(&b))))-1)<1e-4f,"Seamless rotations");
            Check(XMVectorGetX(XMVector3Length(XMLoadFloat3(&track.positions.front().value)-XMLoadFloat3(&track.positions.back().value)))<1e-4f,"Seamless positions");
        }
    }
    LocomotionBlendSpace blend; Check(blend.Initialize(skeleton,clips),"Initialize blend space");
    for (int frame=0;frame<600;++frame) {
        const float angle=frame*.025f;
        const auto* clip=blend.Update(1.0f/60,std::sin(angle)*2.97f,std::cos(angle)*2.97f,1.8f);
        for (const auto& track:clip->tracks) {
            const auto& q=track.rotations[0].value;
            Check(std::abs(XMVectorGetX(XMVector4Length(XMLoadFloat4(&q)))-1)<1e-4f,"Normalised blended rotations");
        }
    }
    // The shipped path: the Mixamo-rigged bandit adopts four authored cycles
    // instead of baking its run slots. The bake above only covers the IK
    // fallback, so without this the arrangement the game actually loads goes
    // untested.
    {
        const std::string mixamoDir=std::string(argv[1])+
            "/Content/Models/MilitaryMercenaryBandit/Mixamo/";
        Assimp::Importer meshImporter;
        const aiScene* meshScene=meshImporter.ReadFile(mixamoDir+"SK_BanditMixamo.fbx",0);
        Check(meshScene && meshScene->mRootNode,"Load the Mixamo-rigged bandit");
        Skeleton mixamo; Bones(meshScene->mRootNode,-1,mixamo);
        XMStoreFloat4x4(&mixamo.globalInverse,
            XMMatrixInverse(nullptr,XMLoadFloat4x4(&mixamo.localBind[0])));

        // Idle and the two gaits still come from the UE4 demo clips, exactly
        // as main.cpp loads them: they resolve onto this rig by bone name and
        // give the bake the sources it needs for the slots left over.
        std::vector<AnimationClip> adopted;
        for (const char* name : {"Idle","Walk","Run"}) {
            Assimp::Importer importer;
            const aiScene* sc=importer.ReadFile(
                std::string(argv[1])+"/Content/Models/MilitaryMercenaryBandit/"
                "Animations/Demo/ThirdPerson"+name+".FBX",0);
            AnimationClip clip;
            if (ReadClip(sc,mixamo,std::string("ThirdPerson")+name,clip))
                adopted.push_back(std::move(clip));
        }
        Check(adopted.size()==3,"Load the gait sources onto the Mixamo rig");
        const char* files[][2]={
            {"SK_BanditMixamo.fbx","SK_BanditMixamo"},
            {"Animations/RunBackward.fbx","RunBackwardSource"},
            {"Animations/RunLeft.fbx","RunLeftSource"},
            {"Animations/RunRight.fbx","RunRightSource"}};
        for (auto& f : files) {
            Assimp::Importer importer;
            const aiScene* sc=importer.ReadFile(mixamoDir+f[0],0);
            AnimationClip clip;
            if (ReadClip(sc,mixamo,f[1],clip)) adopted.push_back(std::move(clip));
        }
        Check(adopted.size()==7,"Load the four authored cycles");

        const std::vector<DirectionalLocomotion::AuthoredCycle> cycles={
            {"SK_BanditMixamo",true,true,0},{"RunBackwardSource",true,true,1},
            {"RunLeftSource",true,true,2},{"RunRightSource",true,true,3}};
        Check(DirectionalLocomotion::Bake(mixamo,adopted,cycles),
              "Bake the adopted cycles");

        // Adoption replaces a clip in place for its own gait but appends for
        // the gait that borrows it, so a total count says little. What has to
        // hold is that every slot the blend space indexes exists exactly once.
        for (int slot=0;slot<8;++slot) {
            int found=0;
            for (const auto& clip:adopted)
                if (clip.name==DirectionalLocomotion::Names[slot]) ++found;
            if (found!=1) std::cerr<<DirectionalLocomotion::Names[slot]<<": "<<found<<std::endl;
            Check(found==1,"Each directional slot is filled exactly once");
        }

        LocomotionBlendSpace adoptedBlend;
        Check(adoptedBlend.Initialize(mixamo,adopted,false),
              "Initialize the blend space over the adopted cycles");
        for (const auto& clip:adopted) {
            bool slot=false;
            for (int i=0;i<8;++i) slot=slot||clip.name==DirectionalLocomotion::Names[i];
            if (!slot) continue;
            for (const auto& track:clip.tracks) {
                if (track.rotations.empty()) continue;
                const auto& a=track.rotations.front().value;
                const auto& b=track.rotations.back().value;
                Check(std::abs(std::abs(XMVectorGetX(XMVector4Dot(
                          XMLoadFloat4(&a),XMLoadFloat4(&b))))-1)<1e-3f,
                      "Adopted cycles loop seamlessly");
            }
        }
        for (int frame=0;frame<600;++frame) {
            const float angle=frame*.025f;
            const auto* clip=adoptedBlend.Update(
                1.0f/60,std::sin(angle)*2.97f,std::cos(angle)*2.97f,1.8f);
            for (const auto& track:clip->tracks) {
                const auto& q=track.rotations[0].value;
                Check(std::abs(XMVectorGetX(XMVector4Length(XMLoadFloat4(&q)))-1)<1e-4f,
                      "Normalised rotations over the adopted blend");
            }
        }

        // Mixamo's authored-only arrangement has no generic Walk/Run clips.
        // Use renamed copies so the test also proves BakeAuthored leaves its
        // source inventory untouched while producing both gaits.
        std::vector<AnimationClip> authoredOnly;
        const char* sourceNames[4] = {
            "SK_BanditMixamo", "RunBackwardSource", "RunLeftSource", "RunRightSource"};
        std::vector<std::string> originalNames;
        for (int direction=0; direction<4; ++direction) {
            auto it=std::find_if(adopted.begin(),adopted.end(),[&](const AnimationClip& c) {
                return c.name==sourceNames[direction];
            });
            Check(it!=adopted.end(),"Find authored-only source");
            authoredOnly.push_back(*it);
            authoredOnly.back().name="AuthoredSource"+std::to_string(direction);
            originalNames.push_back(authoredOnly.back().name);
        }
        const std::vector<DirectionalLocomotion::AuthoredCycle> authored={
            {"AuthoredSource0",true,false,0},{"AuthoredSource1",true,false,1},
            {"AuthoredSource2",true,false,2},{"AuthoredSource3",true,false,3}};
        Check(DirectionalLocomotion::BakeAuthored(mixamo,authoredOnly,authored),
              "Bake authored-only Y-up cycles");
        Check(authoredOnly.size()==12,"Retain four authored sources and add eight slots");
        for (int direction=0; direction<4; ++direction) {
            Check(authoredOnly[direction].name==originalNames[direction],
                  "Preserve authored source names");
            const auto walkIt=std::find_if(authoredOnly.begin(),authoredOnly.end(),[&](const AnimationClip& c) {
                return c.name==DirectionalLocomotion::Names[direction];
            });
            const auto runIt=std::find_if(authoredOnly.begin(),authoredOnly.end(),[&](const AnimationClip& c) {
                return c.name==DirectionalLocomotion::Names[4+direction];
            });
            Check(walkIt!=authoredOnly.end() && runIt!=authoredOnly.end(),
                  "Find authored-only slots");
            const auto& walk=*walkIt;
            const auto& run=*runIt;
            Check(std::abs(walk.duration/run.duration-1.5f)<1e-4f,
                  "Retime authored run as walk");
        }
    }
    std::cout<<"Directional locomotion: weights, real-asset bake, adopted cycles,"
               " loop seams and rotating blend passed"<<std::endl;
}
