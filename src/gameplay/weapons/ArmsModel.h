#pragma once

// The first-person view model: a full rifle-idle character, GPU-skinned and
// animating, drawn in the weapon's own local space so the body and the gun move
// as one unit.
//
// This is the real skinned path, not a baked pose. An earlier version sampled a
// single frame and froze it into static geometry, which is cheaper but shows a
// dead mannequin -- the clip here is a breathing idle and is meant to play. So
// the skeleton is kept, the clip is advanced every frame, and the bone palette
// is uploaded per frame exactly the way SkinnedEnemy does it.
//
// Notes specific to this asset:
//
//   * IT IS A WHOLE CHARACTER, roughly 178 cm tall with legs, hips, neck, head
//     and eyelashes -- not a pair of floating FPS arms. Only the part in front
//     of the camera is ever seen, so it is scaled and pushed back until the
//     arms and weapon frame correctly and the rest falls behind the near plane.
//
//   * The rig is Mixamo's (`mixamorig:` bone names) and self-consistent: all 22
//     weighted bones are animated. The previous Unreal-skeleton arms asset had
//     five unanimated fingertips, which is what tore its hands apart; nothing
//     here needs that repair.
//
//   * Assimp splits FBX node transforms into synthetic
//     `<bone>_$AssimpFbx$_Rotation` nodes. SkinnedFBXImporter registers those in
//     the skeleton and resolves the clip's tracks against them by name, so the
//     split is handled and no special casing is needed here.

#include "AnimationRuntime.h"
#include "DX12Core.h"
#include "GLBImporter.h"
#include "MeshShaderDX12.h"
#include "ProceduralRunAnimation.h"
#include "ShaderDX12.h"
#include "SkinnedFBXImporter.h"
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace DirectX;

extern MeshShaderDX12 g_meshShader;

class ArmsModel {
public:
    static bool Loaded() { return Source().valid && Source().node != nullptr; }

    // Drawn unless the player turns it off from the debug UI.
    static bool& Visible() {
        static bool visible = ArmsEnabledByDefault();
        return visible;
    }

    // Escape hatch: SGE_NO_ARMS=1 loads the model but never draws it, which
    // isolates whether a rendering problem comes from this mesh.
    static bool ArmsEnabledByDefault() {
        size_t length = 0;
        char value[8] = {};
        if (getenv_s(&length, value, sizeof(value), "SGE_NO_ARMS") != 0) return true;
        return length == 0 || value[0] == '0';
    }

    // Placement relative to the weapon, in gun-local units. The character is
    // rebased so the origin sits at the head (see Normalise), i.e. at the
    // camera, so these offsets slide the whole body from there. The default
    // drops it slightly and pushes it back so the shoulders sit behind the eye
    // rather than across it. Live-tunable from the engine UI, like the ADS
    // offsets.
    // Tuned against the render rather than derived: these are the values the
    // arms actually sit correctly at on the weapon.
    static XMFLOAT3& Offset() {
        static XMFLOAT3 offset = { -0.261f, 0.019f, -0.171f };
        return offset;
    }
    // Extra run-only pull toward the camera. Positive values move the animated
    // arms backwards along the gun's local Z axis without disturbing idle.
    static float& RunBackOffset() {
        static float offset = 0.0f;
        return offset;
    }
    static float& Scale() {
        static float scale = 1.00f;
        return scale;
    }
    // Yaw/pitch/roll in degrees, applied in the model's own space before it is
    // placed on the weapon. The character faces down its own +Z; whether that
    // lines up with the barrel depends on the export, so this stays adjustable.
    static XMFLOAT3& Rotation() {
        static XMFLOAT3 rotation = { 0.0f, -9.0f, -20.0f };
        return rotation;
    }

    static SkinnedModel& Source() {
        static SkinnedModel source;
        return source;
    }

    // Model space (native centimetres, origin at the feet) -> gun-local space
    // (the frame the weapon mesh is laid out in, before the weapon's own scale).
    // Shared by the mesh draw and the weapon anchor so the two cannot drift.
    // Mirror the character left-to-right. The clip holds the rifle in the hand
    // opposite the one this game's weapon is drawn for, and a mirror swaps the
    // whole body -- trigger hand, support hand and stance together -- which no
    // amount of rotation can do.
    //
    // Negating X makes the transform left-handed, so triangle winding reverses
    // and back-face culling would show the inside of the model. The draw
    // compensates by rendering the view model double-sided, which it already
    // does because the camera sits close enough to clip into a sleeve.
    static bool& MirrorX() {
        static bool mirror = true;
        return mirror;
    }

    // Which hand the weapon is aligned to. Mirroring swaps the sides on screen,
    // so the bone that ends up in the trigger position is the opposite of the
    // one named in the rig -- with the mirror on, the rig's LEFT hand is the
    // hand the player sees on the right, holding the gun.
    static bool& GripUsesLeftHand() {
        static bool left = true;
        return left;
    }

    static XMMATRIX ModelToGunLocal() {
        const XMFLOAT3& offset = Offset();
        const XMFLOAT3& rotation = Rotation();
        const float scale = Scale() * NormaliseScale();
        const float mirror = MirrorX() ? -1.0f : 1.0f;
        const float runBack = RunBlendWeight() * RunBackOffset();
        // Per-weapon nudge rides here rather than in the alignment solve: this
        // is a pure delta on the already-tuned offset, so a weapon left at zero
        // produces exactly the transform it did before the table existed.
        const XMFLOAT3& grip = PlayerGripOffset();
        return XMMatrixTranslation(-Pivot().x, -Pivot().y, -Pivot().z) *
               XMMatrixRotationRollPitchYaw(XMConvertToRadians(rotation.x),
                                            XMConvertToRadians(rotation.y),
                                            XMConvertToRadians(rotation.z)) *
               XMMatrixScaling(scale * mirror, scale, scale) *
               XMMatrixTranslation(offset.x + grip.x, offset.y + grip.y,
                                   offset.z + grip.z - runBack);
    }

    // Hide the character's own head. The camera sits at the eyes, so the skull
    // wraps around the near plane and fills the screen with the inside of the
    // face -- which is exactly what it did before this. Every FPS view model
    // solves it the same way: the head simply is not drawn.
    // Off by default now: the asset is the character with everything but the
    // arms already deleted, so there is no head geometry left to suppress.
    // Kept as a toggle in case a future asset needs it again.
    static bool& HideHead() {
        static bool hide = false;
        return hide;
    }

    // Suppress the wrist and fingers opposite the hand pinned to the weapon.
    // Default grip uses the rig's left hand, so this hides the rig's right hand.
    static bool& HideFreeHand() {
        static bool hide = true;
        return hide;
    }

    // Play/pause the idle. Held still by default: a moving body is impossible to
    // align a fixed weapon against, because the hands are somewhere different
    // every frame. Get the pose sitting on the gun first, then turn this on.
    static bool& Animate() {
        static bool animate = true;
        return animate;
    }

    // Frame the pose is frozen at while paused, in seconds into the clip.
    // Different moments of a breathing idle hold the rifle slightly differently,
    // so this picks which one to align against.
    static float& PoseTime() {
        static float time = 0.0f;
        return time;
    }

    // Horizontal speed where the additive run layer reaches full strength.
    // Zero makes any actual movement drive the layer to full strength.
    static float& RunSpeedThreshold() {
        static float speed = 0.0f;
        return speed;
    }

    static float& RunBlendWeight() {
        static float weight = 0.0f;
        return weight;
    }

    static bool Running() { return RunBlendWeight() >= 0.5f; }

    // Idle remains the base pose. Run supplies only motion relative to its first
    // frame, blended smoothly by player speed.
    static void Update(float deltaTime, float playerHorizontalSpeed = 0.0f,
                       float adsBlend = 0.0f, bool sprinting = false) {
        if (!Loaded()) return;
        UpdateRunBlend(deltaTime, playerHorizontalSpeed);
        if (Animate()) {
            Animation().Advance(deltaTime);
            // Sprinting doubles the run cycle; jogging plays it at authored
            // speed. Only the run clip is scaled -- the idle underneath keeps
            // real time, so breathing and sway do not speed up with the legs.
            // Scaling the advance rather than the clip leaves the authored data
            // untouched and keeps the loop blend working on its own duration.
            if (RunAnimation().clip && RunBlendWeight() > 0.0001f) {
                const float rate = sprinting ? kSprintPlaybackRate : 1.0f;
                RunAnimation().Advance(deltaTime * rate);
            }
        } else {
            // Re-sampling every frame costs little and keeps the pose live while
            // PoseTime is dragged in the UI.
            Animation().time = PoseTime();
            RunAnimation().time = 0.0f;
        }
        const float sighted =
            (std::max)(0.0f, (std::min)(1.0f, adsBlend));
        const float sightedRunScale = 1.0f - 0.70f * sighted;
        const float idleMotionRange =
            1.0f - (1.0f - kAdsIdleMotionRange) * sighted;
        Animation().ComputeAdditivePalette(
            Source().skeleton, RunAnimation(), 0.0f,
            RunBlendWeight() * sightedRunScale * kProceduralRunStrength,
            PaletteCPU(), &PoseGlobals(), idleMotionRange, 0.0f);
        if (HideHead() || HideFreeHand()) CollapseHiddenBones();
    }

    // Per-frame palette upload. One buffer per in-flight frame so a palette the
    // GPU is still reading is never overwritten.
    static D3D12_GPU_VIRTUAL_ADDRESS UploadPalette() {
        if (PaletteCPU().empty() || !Palette()[0]) return 0;
        const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
        std::memcpy(Mapped()[frame], PaletteCPU().data(), PaletteBytes());
        return Palette()[frame]->GetGPUVirtualAddress();
    }

    // Draw the skinned view model. `base` is the weapon's own transform, so the
    // body inherits recoil, the ADS slide and the hip offset for free.
    static void Draw(ShaderDX12& shader, const XMMATRIX& base,
                     const XMMATRIX& view, const XMMATRIX& proj,
                     const XMMATRIX& lightSpace, float weaponScale) {
        if (!Loaded() || !Visible() || PaletteCPU().empty()) return;
        const D3D12_GPU_VIRTUAL_ADDRESS paletteAddress = UploadPalette();
        if (!paletteAddress) return;

        const XMMATRIX world = ModelToGunLocal() * XMMatrixScaling(
            weaponScale, weaponScale, weaponScale) * base;
        shader.SetMatrices(world, view, proj, lightSpace);

        for (const MeshPrimitive& primitive : Source().node->mesh->primitives) {
            if (primitive.vbv.BufferLocation == 0 || !primitive.skinBuffer) continue;
            shader.Use(false);
            if (primitive.material) {
                const XMFLOAT3 color(primitive.material->baseColorFactor.x,
                                     primitive.material->baseColorFactor.y,
                                     primitive.material->baseColorFactor.z);
                shader.SetObjectMaterial(color,
                    primitive.material->baseColorTexture != nullptr,
                    primitive.material->normalTexture != nullptr,
                    primitive.material->metallicFactor,
                    primitive.material->roughnessFactor,
                    primitive.material->baseColorTexture.Get(),
                    primitive.material->normalTexture.Get(),
                    primitive.material->metallicRoughnessTexture.Get(),
                    primitive.material->roughnessOnlyTexture, 1.0f,
                    primitive.material->alphaCutout,
                    primitive.material.get(),
                    primitive.material->alphaFromLuminance,
                    primitive.material->ambientScale,
                    primitive.material->occlusionStrength,
                    primitive.material->normalYSign,
                    primitive.material->viewFillStrength);
            } else {
                shader.SetObjectColor(XMFLOAT3(0.7f, 0.7f, 0.72f));
            }

            const D3D12_GPU_VIRTUAL_ADDRESS descAddress =
                primitive.meshletDescBuffer ? primitive.meshletDescBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS boundsAddress =
                primitive.meshletBoundsBuffer ? primitive.meshletBoundsBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS vertexIndexAddress =
                primitive.meshletVertexIndexBuffer ? primitive.meshletVertexIndexBuffer->GetGPUVirtualAddress() : 0;
            const D3D12_GPU_VIRTUAL_ADDRESS triangleAddress =
                primitive.meshletTriangleBuffer ? primitive.meshletTriangleBuffer->GetGPUVirtualAddress() : 0;
            if (!shader.IsViewmodelPassActive() &&
                g_meshShader.CanDraw(primitive.meshletCount, descAddress,
                                     boundsAddress, vertexIndexAddress, triangleAddress)) {
                // Material setup can switch the shared root signature between
                // legacy tables and bindless indices. Match the mesh PSO to it
                // for every primitive instead of inheriting the last scene mesh.
                g_meshShader.SetBindlessActive(shader.BindlessDrawActive());
                g_meshShader.Draw(primitive.vbv,
                    static_cast<UINT>(primitive.vertices.size() / 12),
                    primitive.indexCount, primitive.meshletCount,
                    descAddress, boundsAddress, vertexIndexAddress, triangleAddress,
                    paletteAddress, primitive.skinBuffer->GetGPUVirtualAddress(),
                    // Double-sided: at this range the camera can end up just
                    // inside a sleeve or collar, and a back-facing triangle
                    // there would open a hole straight through the character.
                    true,
                    // No occlusion test -- the depth pyramid is a frame behind,
                    // and geometry this close to the eye changes too much
                    // between frames for it to be safe.
                    false,
                    // No meshlet culling: bind-pose bounds do not describe the
                    // posed view model drawn at the camera.
                    true);
            } else if (shader.IsViewmodelPassActive()) {
                // Smooth see-through uses matching depth-only and alpha IA
                // PSOs. The mesh pipeline has independent fixed-function state,
                // so using it here would bypass the nearest-surface prepass.
                shader.SetSkinningEnabled(true);
                g_dx12.commandList->SetGraphicsRootShaderResourceView(
                    16, paletteAddress);
                g_dx12.commandList->SetGraphicsRootShaderResourceView(
                    17, primitive.skinBuffer->GetGPUVirtualAddress());
                g_dx12.commandList->SetGraphicsRootShaderResourceView(
                    19, paletteAddress);
                g_dx12.commandList->SetPipelineState(
                    shader.GetPipelineState(false));
                g_dx12.commandList->IASetVertexBuffers(0, 1, &primitive.vbv);
                g_dx12.commandList->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                if (primitive.ibv.BufferLocation != 0) {
                    g_dx12.commandList->IASetIndexBuffer(&primitive.ibv);
                    g_dx12.commandList->DrawIndexedInstanced(
                        primitive.indexCount, 1, 0, 0, 0);
                } else {
                    g_dx12.commandList->DrawInstanced(
                        static_cast<UINT>(primitive.vertices.size() / 12),
                        1, 0, 0);
                }
            }
            shader.NextDrawCall();
        }
        if (shader.IsViewmodelPassActive())
            shader.SetSkinningEnabled(false);
    }

    // Load the view model. Safe to call repeatedly; only the first call does
    // anything. Must run inside the model-loading command-list window -- it
    // records texture uploads.
    static void Load() {
        static bool attempted = false;
        if (attempted) return;
        attempted = true;

        const std::string path =
            Resolve("Content/Models/MainPlayer/ArmsOnly/PlayerArms.fbx");
        std::cout << "Loading FPS view model " << path << "...\n";

        // The rifle idle ships as a separate animation-only FBX (no meshes, one
        // clip) that shares this rig's mixamorig bone names, so the importer can
        // retarget it onto the arms by name. Loading it here rather than relying
        // on the clips baked into PlayerArms.fbx: those are Blender's per-object
        // action leftovers, six of which are two-key stubs.
        const std::string animationPath =
            Resolve("Content/Models/MainPlayer/ArmsOnly/Rifle Aiming Idle(1).fbx");
        std::vector<std::string> animationPaths;
        if (std::filesystem::exists(animationPath))
            animationPaths.push_back(animationPath);
        else
            std::cerr << "Rifle idle clip missing: " << animationPath << "\n";

        // uniformScale 1 keeps the mesh in the skeleton's native centimetres,
        // which is what the palette matrices are built in; the conversion to
        // weapon-local size happens on the world matrix in Draw().
        // useCookedClips=false: the cooked rifle idle came back with 4 bone
        // tracks instead of 66, so 83 of the 87 bones stayed in bind pose and
        // the arms hung off the weapon. Parse the source FBX, which carries the
        // full track set. Nothing about the player is loaded from cooked data.
        Source() = SkinnedFBXImporter::Load(
            path, animationPaths, g_dx12.device, g_dx12.commandList, 1.0f,
            false);
        if (!Source().valid || !Source().node || !Source().node->mesh) {
            std::cerr << "FPS view model unavailable; weapon draws without arms\n";
            return;
        }
        if (Source().clips.empty()) {
            std::cerr << "FPS view model has no animation clip\n";
        }
        ProceduralRunClip() =
            ProceduralRunAnimation::Build(Source().skeleton);
        DropUnskinnedPrimitives();
        ApplyEmbeddedTextures();
        if (!CreatePaletteBuffers()) {
            std::cerr << "FPS view model palette buffers failed\n";
            return;
        }

        // Pose first, THEN measure. Normalise() reads joint positions out of
        // PoseGlobals, so the clip has to be sampled before it runs.
        if (const AnimationClip* clip = IdleClip()) Animation().Play(clip);
        if (const AnimationClip* clip = RunClip()) {
            RunAnimation().Play(clip);
            RunAnimation().loopBlendDuration = 0.08f;
        }
        Animation().time = PoseTime();
        RunAnimation().time = 0.0f;
        RunBlendWeight() = 0.0f;
        Animation().ComputeAdditivePalette(
            Source().skeleton, RunAnimation(), 0.0f, 0.0f,
            PaletteCPU(), &PoseGlobals());

        // The reference pose the weapon's motion is measured against. Captured
        // from the same frame the body was aligned at, so at that frame the
        // weapon sits exactly where it was tuned and the animation moves it
        // from there.
        BindGlobals() = PoseGlobals();

        Normalise();
        FindGripBone();
        FindFollowBone();
        FindHiddenBones();
        // Keep the artist-tuned startup Offset. Alignment remains available
        // through the UI for fitting another pose or asset.
        if (HideHead() || HideFreeHand()) CollapseHiddenBones();

        if (FILE* file = std::fopen("arms_load.log", "w")) {
            size_t triangles = 0, vertices = 0, skinned = 0;
            for (const MeshPrimitive& primitive : Source().node->mesh->primitives) {
                triangles += primitive.indices.size() / 3;
                vertices += primitive.vertices.size() / 12;
                if (primitive.skinBuffer) ++skinned;
            }
            std::fprintf(file,
                "loaded=1 prims=%zu skinnedPrims=%zu verts=%zu tris=%zu bones=%zu\n",
                Source().node->mesh->primitives.size(), skinned, vertices,
                triangles, Source().skeleton.BoneCount());
            std::fprintf(file, "droppedUnskinned=%zu\n", s_droppedPrimitives);
            std::fprintf(file, "clips=%zu chosen='%s'\n", Source().clips.size(),
                         Animation().clip ? Animation().clip->name.c_str() : "none");
            for (const AnimationClip& clip : Source().clips) {
                size_t keys = 0;
                for (const BoneTrack& track : clip.tracks)
                    keys += track.positions.size() + track.rotations.size() +
                            track.scales.size();
                std::fprintf(file, "  '%s' %.2fs tracks=%zu keys=%zu%s\n",
                             clip.name.c_str(), clip.duration, clip.tracks.size(),
                             keys, Animation().clip == &clip ? "  <-- playing" : "");
            }
            std::fprintf(file, "pivot=(%.2f,%.2f,%.2f) normaliseScale=%.5f\n",
                         Pivot().x, Pivot().y, Pivot().z, NormaliseScale());
            std::fprintf(file, "offset=(%.3f,%.3f,%.3f) gripTarget=(%.3f,%.3f,%.3f)\n",
                         Offset().x, Offset().y, Offset().z,
                         WeaponGrip().x, WeaponGrip().y, WeaponGrip().z);
            if (GripBone() >= 0 && static_cast<size_t>(GripBone()) < PoseGlobals().size()) {
                const XMMATRIX hand =
                    XMLoadFloat4x4(&PoseGlobals()[GripBone()]) * ModelToGunLocal();
                XMFLOAT3 handPosition;
                XMStoreFloat3(&handPosition, hand.r[3]);
                std::fprintf(file, "handAtLoad=(%.3f,%.3f,%.3f)\n",
                             handPosition.x, handPosition.y, handPosition.z);
            }
            std::fprintf(file, "skeletonBones=%zu firstNames:", Source().skeleton.names.size());
            for (size_t b = 0; b < Source().skeleton.names.size() && b < 10; ++b)
                std::fprintf(file, " '%s'", Source().skeleton.names[b].c_str());
            std::fprintf(file, "\n");
            std::fprintf(file, "pivotSource=%s\n",
                         Pivot().x == 0.0f && Pivot().y == 0.0f && Pivot().z == 0.0f
                             ? "ORIGIN (no anchor bone found)" : "bone");
            std::fprintf(file, "followBone=%d (%s) bindGlobals=%zu\n", FollowBone(),
                         FollowBone() >= 0 &&
                             static_cast<size_t>(FollowBone()) < Source().skeleton.names.size()
                             ? Source().skeleton.names[FollowBone()].c_str() : "none",
                         BindGlobals().size());
            std::fprintf(file, "gripBone=%d (%s) hiddenBones=%zu\n", GripBone(),
                         GripBone() >= 0 &&
                             static_cast<size_t>(GripBone()) < Source().skeleton.names.size()
                             ? Source().skeleton.names[GripBone()].c_str() : "none",
                         HiddenBones().size() + FreeHandBones().size());
            std::fprintf(file, "bounds x[%.2f..%.2f] y[%.2f..%.2f] z[%.2f..%.2f]\n",
                         s_lo.x, s_hi.x, s_lo.y, s_hi.y, s_lo.z, s_hi.z);
            std::fclose(file);
        }
        std::cout << "FPS view model loaded\n";
    }

    // Drop staging memory once the loader has flushed, mirroring the
    // ReleaseMaterialUploadHeaps pass every other model goes through.
    static void ReleaseUploadHeaps() {
        for (const auto& material : Source().materialKeepAlive)
            if (material) material->uploadHeaps.clear();
        if (Source().node && Source().node->mesh)
            for (MeshPrimitive& primitive : Source().node->mesh->primitives)
                if (primitive.material) primitive.material->uploadHeaps.clear();
    }

    static AnimationInstance& Animation() {
        static AnimationInstance animation;
        return animation;
    }
    static AnimationInstance& RunAnimation() {
        static AnimationInstance animation;
        return animation;
    }

    // Where on the weapon the aligned hand is placed. Live-tunable so the fit
    // can be dialled in against the actual render instead of rebuilt per nudge;
    // see the comment on the constant below for what the numbers mean.
    static XMFLOAT3& WeaponGrip() {
        static XMFLOAT3 grip = { -0.050f, -0.130f, 0.475f };
        return grip;
    }

    // Highest weapon id the per-weapon table covers. Mirrors GunModel's
    // kMaxWeapon, spelled out rather than included: ArmsModel is deliberately
    // unaware of GunModel, so the weapon index is passed in from outside.
    static constexpr int kMaxGripWeapon = 10;

    // Per-weapon nudge ADDED to the shared grip point above, in gun-local
    // units. Zero means "sits exactly where the shared value puts it", which is
    // every weapon that has not needed correcting -- the shared point stays the
    // baseline for all of them, and this only carries the difference a
    // particular rifle needs.
    //
    // With GripUsesLeftHand (the default) this adjusts the LEFT arm.
    static XMFLOAT3& WeaponGripOffset(int weapon) {
        static std::array<XMFLOAT3, kMaxGripWeapon + 1> offsets = {{
            { 0.000f,  0.000f, 0.0f }, // AK47
            // Support hand forward onto the pump, which sits further down
            // the barrel than the AK handguard the shared grip was set on.
            {-0.025f, -0.010f, 0.070f }, // Remington 870
            { 0.000f,  0.000f, 0.0f }, // RPG
            { 0.000f,  0.000f, 0.0f }, // SVD
            { 0.000f,  0.000f, 0.0f }, // laser
            { 0.000f,  0.000f, 0.0f }, // C4
            { 0.000f,  0.000f, 0.0f }, // flamethrower
            { 0.000f,  0.000f, 0.0f }, // harpoon
            { 0.000f,  0.000f, 0.0f }, // SVD suppressed
            // The M4's handguard is shorter and sits lower than the AK's, so
            // the shared grip point leaves the support hand off the rail.
            {-0.030f, -0.030f, 0.0f }, // M4A1
            { 0.000f,  0.000f, 0.0f }, // AK-74: AK pattern, so no nudge
        }};
        const int slot = (std::max)(0, (std::min)(weapon, kMaxGripWeapon));
        return offsets[static_cast<size_t>(slot)];
    }

    static XMFLOAT3 DefaultWeaponGripOffset(int weapon) {
        if (weapon == 1) return { -0.025f, -0.010f, 0.070f };
        if (weapon == 9) return { -0.030f, -0.030f, 0.000f };
        return { 0.0f, 0.0f, 0.0f };
    }

    // Which weapon the per-weapon nudge is read for. Pushed in by the caller as
    // the selection changes, so this file need not know what a weapon is.
    static int& GripWeapon() {
        static int weapon = 0;
        return weapon;
    }
    static XMFLOAT3& PlayerGripOffset() {
        return WeaponGripOffset(GripWeapon());
    }


    // Re-solve the body offset so the aligned hand sits on the weapon's grip
    // point. Exposed for the debug UI: after scrubbing to a different frame of
    // the idle, or moving the grip target, this snaps the body back onto the gun.
    static void RealignHandsToWeapon() {
        AlignHandsToWeapon();
        BindGlobals() = PoseGlobals();
    }

    // Point the grip table at a different weapon.
    //
    // Deliberately does NOT re-solve the alignment. AlignHandsToWeapon measures
    // the hand in the CURRENT animated frame, so calling it while the idle plays
    // solves against a different hand position than the one the tuned Offset()
    // was derived from -- the body then jumps, whether or not this weapon
    // carries a nudge. The nudge is applied as a delta in ModelToGunLocal
    // instead, which leaves the tuned placement exactly as it is.
    static void SetGripWeapon(int weapon) {
        GripWeapon() = weapon;
    }

    // Let the weapon ride the hand instead of hanging at a fixed spot.
    //
    // The body is placed by pinning the hand that holds the rifle to a fixed
    // point on it, which is what makes a static pose line up. Once the idle
    // plays, that hand breathes -- and a weapon nailed to gun-local space would
    // stay put while the hand drifts off it. Tracking that same hand's
    // displacement keeps the rifle in the grip for the whole clip.
    static bool& WeaponFollowsHand() {
        static bool follow = true;
        return follow;
    }

    // Fixed corrections applied on top of the motion the hand contributes,
    // since the wrist bone's axes are the rigger's and have nothing to do with
    // how the AK is laid out. Live-tunable.
    static XMFLOAT3& FollowOffset() {
        static XMFLOAT3 offset = { 0.0f, 0.0f, 0.0f };
        return offset;
    }
    static XMFLOAT3& FollowRotation() {
        static XMFLOAT3 rotation = { 0.0f, 0.0f, 0.0f };
        return rotation;
    }

    // Weapon transform in gun-local space, tracking the trigger hand. False when
    // there is no pose yet or the feature is off, in which case the renderer
    // keeps the weapon at its fixed camera-relative spot.
    //
    // The motion is applied as a DELTA from the bind pose, not as the bone's
    // absolute transform. Absolute would drag the rifle to wherever the wrist
    // sits in the model's own space -- metres away, and it would ignore all the
    // hand-tuned placement the weapon already has. The delta keeps the gun where
    // it was authored and only adds the movement the animation introduces.
    static bool WeaponFollowTransform(XMMATRIX& out, float weaponScale) {
        if (!WeaponFollowsHand() || !Loaded() || !Visible()) return false;
        const int bone = FollowBone();
        if (bone < 0 || static_cast<size_t>(bone) >= PoseGlobals().size() ||
            static_cast<size_t>(bone) >= BindGlobals().size())
            return false;

        // Track wrist LOCATION only. Wrist rotation axes differ from weapon
        // axes, and applying them twists the rifle. Measuring both poses after
        // ModelToGunLocal handles arm rotation, mirroring, pivot, and scale.
        const XMMATRIX modelToGun = ModelToGunLocal();
        const XMMATRIX bindHand =
            XMLoadFloat4x4(&BindGlobals()[bone]) * modelToGun;
        const XMMATRIX poseHand =
            XMLoadFloat4x4(&PoseGlobals()[bone]) * modelToGun;
        const XMVECTOR translation = poseHand.r[3] - bindHand.r[3];
        const XMFLOAT3& offset = FollowOffset();
        const XMFLOAT3& rotation = FollowRotation();
        const float s = (std::max)(0.0f, weaponScale);
        XMFLOAT3 delta;
        XMStoreFloat3(&delta, translation);
        out = XMMatrixRotationRollPitchYaw(XMConvertToRadians(rotation.x),
                                           XMConvertToRadians(rotation.y),
                                           XMConvertToRadians(rotation.z)) *
              XMMatrixTranslation((delta.x + offset.x) * s,
                                  (delta.y + offset.y) * s,
                                  (delta.z + offset.z) * s);
        return true;
    }

    // Re-resolve the grip bone and snap the body back onto the weapon. For the
    // debug UI, after switching which hand holds the gun.
    static void RebindGripBone() {
        FindGripBone();
        FindFollowBone();
        FindHiddenBones();
        AlignHandsToWeapon();
        BindGlobals() = PoseGlobals();
    }

    // Upload resolution for the two maps that are actually sampled. The source
    // is 4K; at view-model size 1024 is indistinguishable and costs 4 MB rather
    // than 64 MB per map.
    static constexpr int kAlbedoTextureSize = 1024;
    static constexpr int kNormalTextureSize = 1024;

    // Defined in ArmsTextures.cpp: this asset embeds its maps inside the FBX
    // under paths that only existed on the exporter's machine, so they have to
    // be pulled from the file rather than resolved on disk.
    static void ApplyEmbeddedTextures();

    // Exposed for ArmsTextures.cpp, which needs the same search behaviour.
    static std::string ResolvePath(const std::string& rel) {
        for (const std::string& c : { rel, "build/" + rel, "../" + rel, "../../build/" + rel })
            if (std::filesystem::exists(c)) return c;
        return rel;
    }

private:
    // Where the hands sit in the mesh's own space, and how big the model should
    // be against the weapon. Filled by Normalise().
    static XMFLOAT3& Pivot() {
        static XMFLOAT3 pivot = { 0.0f, 0.0f, 0.0f };
        return pivot;
    }
    static float& NormaliseScale() {
        static float scale = 1.0f;
        return scale;
    }

    // Length of the upper arm + forearm + hand, in gun-local units.
    //
    // Sizing off the mesh's bounding height does not work here. That assumes the
    // geometry is a whole body, and this asset is a pair of arms cut out of one:
    // dropping the leftover meshes shrank its bounds from 167 units to 77, which
    // -- normalised to a fixed height -- scaled the arms up 2.2x and filled the
    // screen with them. Measuring an actual limb instead is independent of how
    // much of the character survived the edit.
    //
    // The AK is normalised to a 1.25 barrel, so an arm a little over half that
    // reads correctly against the weapon.
    static constexpr float kArmLength = 0.72f;
    static constexpr float kProceduralRunStrength = 1.0f / 3.0f;
    // Run cycle rate while sprinting (shift held). Matches the sprint movement
    // multiplier in the input path -- the legs have to turn over at the same
    // ratio the camera actually moves, or the stride slides against the ground.
    // Change the two together. Normal running keeps the authored 1x.
    static constexpr float kSprintPlaybackRate = 1.5f;
    // ADS keeps only 20% of breathing-idle displacement around frame zero.
    static constexpr float kAdsIdleMotionRange = 0.2f;
    static std::string Resolve(const std::string& rel) { return ResolvePath(rel); }

    static const AnimationClip* IdleClip() {
        if (const AnimationClip* clip =
                Source().FindClip("Rifle Aiming Idle(1)"))
            return clip;
        return RichestClip();
    }

    static const AnimationClip* RunClip() {
        return ProceduralRunClip().tracks.empty()
            ? nullptr : &ProceduralRunClip();
    }

    static AnimationClip& ProceduralRunClip() {
        static AnimationClip clip;
        return clip;
    }

    static void UpdateRunBlend(float deltaTime, float horizontalSpeed) {
        if (!RunAnimation().clip) {
            RunBlendWeight() = 0.0f;
            return;
        }

        const float fullSpeed = (std::max)(0.01f, RunSpeedThreshold());
        const float blendStart = fullSpeed * 0.85f;
        float target = (horizontalSpeed - blendStart) /
                       (fullSpeed - blendStart);
        target = (std::max)(0.0f, (std::min)(1.0f, target));
        target = target * target * (3.0f - 2.0f * target);

        const bool wasActive = RunBlendWeight() > 0.0001f;
        const float response =
            1.0f - std::exp(-10.0f * (std::max)(0.0f, deltaTime));
        RunBlendWeight() += (target - RunBlendWeight()) * response;
        if (std::abs(RunBlendWeight() - target) < 0.0001f)
            RunBlendWeight() = target;
        if (!wasActive && RunBlendWeight() > 0.0001f)
            RunAnimation().time = 0.0f;
    }

    // Pick the clip that actually animates.
    //
    // This export carries seven AnimStacks, but six of them are stubs: two
    // keyframes per track, i.e. a static hold left behind by Blender's
    // per-object action bookkeeping. Only one has real motion (86 keys a bone).
    // Taking clips.front() is a coin flip that mostly loses, so choose by
    // keyframe count instead of order.
    static const AnimationClip* RichestClip() {
        // AppendClips adds the mesh FBX's own stacks first and the external
        // animation files after, so anything past that count came from
        // Rifle Aiming Idle(1).fbx -- the clip we actually want. Prefer it outright:
        // PlayerArms.fbx's embedded 'Armature|Armature' stack has more raw
        // keyframes and would otherwise win the count, even though it is the
        // Blender action left over from authoring rather than the intended idle.
        const AnimationClip* external = nullptr;
        size_t externalKeys = 0;
        const AnimationClip* best = nullptr;
        size_t bestKeys = 0;

        for (size_t i = 0; i < Source().clips.size(); ++i) {
            const AnimationClip& clip = Source().clips[i];
            size_t keys = 0;
            for (const BoneTrack& track : clip.tracks)
                keys += track.positions.size() + track.rotations.size() +
                        track.scales.size();
            // The external clip is named plainly ("mixamo.com"); Blender's
            // embedded actions carry compound "Armature|<object>|..." names.
            const bool isExternal = clip.name.find('|') == std::string::npos;
            if (isExternal && (!external || keys > externalKeys)) {
                external = &clip;
                externalKeys = keys;
            }
            if (!best || keys > bestKeys) {
                best = &clip;
                bestKeys = keys;
            }
        }
        return external ? external : best;
    }

    // Discard geometry that carries no skin weights.
    //
    // PlayerArms.fbx was made by cutting the arms out of the rifle-idle
    // character, and the export also kept the original SK_FPSHands source
    // meshes it was modelled against. Those were never bound to the armature --
    // zero bone weights -- so no pose can move them: they would hang in the air
    // in their authored space while the real arms animate past them. They are
    // also authored Z-up against the rest of the file's Y-up, so they do not
    // even sit in the same place. The skinned Ch_49 meshes ARE the arms.
    static void DropUnskinnedPrimitives() {
        auto& primitives = Source().node->mesh->primitives;
        const size_t before = primitives.size();
        primitives.erase(
            std::remove_if(primitives.begin(), primitives.end(),
                [](const MeshPrimitive& primitive) {
                    if (primitive.skin.empty() || !primitive.skinBuffer) return true;
                    // The importer binds every mesh to the armature, so the
                    // leftovers come back with skin buffers even though the raw
                    // FBX reports zero bone weights for them -- meaning the
                    // weightless test above never fires. Match them by material
                    // instead: MI_Hand01a_* is the SK_FPSHands source geometry
                    // the arms were modelled against, and it is untextured
                    // duplicate that would sit on top of the real arms.
                    if (!primitive.material) return false;
                    std::string name = primitive.material->name;
                    std::transform(name.begin(), name.end(), name.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    return name.find("mi_hand01a") != std::string::npos;
                }),
            primitives.end());
        s_droppedPrimitives = before - primitives.size();
    }

    // Measure the mesh and work out the transform that puts it on the weapon.
    // Nothing is written back into the vertices: they must stay in the space the
    // skeleton's bind matrices were built in, or skinning breaks. Only the
    // pivot and scale used by Draw()'s world matrix are derived here.
    static void Normalise() {
        s_lo = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
        s_hi = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const MeshPrimitive& primitive : Source().node->mesh->primitives)
            for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                s_lo.x = std::min(s_lo.x, primitive.vertices[v]);
                s_hi.x = std::max(s_hi.x, primitive.vertices[v]);
                s_lo.y = std::min(s_lo.y, primitive.vertices[v + 1]);
                s_hi.y = std::max(s_hi.y, primitive.vertices[v + 1]);
                s_lo.z = std::min(s_lo.z, primitive.vertices[v + 2]);
                s_hi.z = std::max(s_hi.z, primitive.vertices[v + 2]);
            }
        // Scale from the skeleton, not the mesh bounds: bone positions are fixed
        // by the rig and do not move when geometry is deleted from the asset.
        const float armLength = MeasureArmLength();
        NormaliseScale() = armLength > 1e-3f ? kArmLength / armLength : 1.0f;

        // Anchor on the HEAD, not the hands. The camera is the character's eyes,
        // so the head bone is the one point that must coincide with it -- the
        // body then hangs below and behind, where a first-person body belongs,
        // and the arms reach forward into view on their own.
        //
        // Anchoring on the hands instead put the head at the camera and left the
        // player looking at the inside of the skull, with the weapon hidden
        // behind the face.
        XMFLOAT3 pivot = { (s_lo.x + s_hi.x) * 0.5f,
                           (s_lo.y + s_hi.y) * 0.5f,
                           (s_lo.z + s_hi.z) * 0.5f };
        // Prefer the head -- it is where the camera sits -- but this asset has
        // had its head deleted, so fall back to the shoulders. Anchoring there
        // still puts the camera above and behind the arms, which is the same
        // relationship, just measured from a joint that still exists.
        XMFLOAT3 anchor = {};
        if (FindBonePoint("head", anchor) ||
            FindBonePoint("shoulder", anchor) ||
            FindBonePoint("neck", anchor))
            pivot = anchor;
        Pivot() = pivot;
    }

    // Bind-pose distance shoulder -> elbow -> wrist, in the model's own units.
    // Walking the actual joints makes the measurement independent of the pose
    // and of how much geometry the asset still has, which a bounding box is not.
    static float MeasureArmLength() {
        XMFLOAT3 shoulder, elbow, wrist;
        const bool haveChain =
            (FindBonePoint("rightarm", shoulder) || FindBonePoint("leftarm", shoulder)) &&
            (FindBonePoint("rightforearm", elbow) || FindBonePoint("leftforearm", elbow)) &&
            (FindBonePoint("righthand", wrist) || FindBonePoint("lefthand", wrist));
        if (!haveChain) {
            // No recognisable arm chain: fall back to the mesh height, which is
            // at least the right order of magnitude.
            return s_hi.y - s_lo.y;
        }
        const XMVECTOR upper = XMVectorSubtract(XMLoadFloat3(&elbow),
                                                XMLoadFloat3(&shoulder));
        const XMVECTOR lower = XMVectorSubtract(XMLoadFloat3(&wrist),
                                                XMLoadFloat3(&elbow));
        return XMVectorGetX(XMVector3Length(upper)) +
               XMVectorGetX(XMVector3Length(lower));
    }

    // Average bind-pose position of every bone whose name contains `keyword`,
    // in mesh space. The skeleton stores inverse-bind matrices, so inverting one
    // gives the bone's bind transform and its translation is the joint position.
    //
    // Finger and Assimp pseudo-node bones are excluded: "hand" also prefixes
    // every finger bone (LeftHandIndex1, ...), and the synthetic
    // "_$AssimpFbx$_" nodes carry partial transforms that would skew the mean.
    static bool FindBonePoint(const char* keyword, XMFLOAT3& out) {
        const Skeleton& skeleton = Source().skeleton;
        XMVECTOR sum = XMVectorZero();
        int found = 0;
        for (size_t b = 0; b < skeleton.names.size(); ++b) {
            std::string name = skeleton.names[b];
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // Match on the part after the rig's "mixamorig:" namespace, and
            // require the whole remainder to match. A plain substring test would
            // make "rightarm" also hit "rightforearm" and average the shoulder
            // together with the elbow.
            const size_t colon = name.find_last_of(':');
            const std::string leaf =
                colon == std::string::npos ? name : name.substr(colon + 1);
            if (leaf != keyword) continue;
            // No finger/pseudo-node exclusions are needed now: an exact leaf
            // match already rejects LeftHandIndex1 and the "_$AssimpFbx$_"
            // helper nodes, which never equal a plain joint name.
            //
            // Read the position from the posed global transform, NOT by
            // inverting the bone's offset matrix. `offset` is the inverse-bind,
            // and BuildSkeleton leaves it as identity for any bone that is not a
            // skin cluster -- which in this arms-only asset includes Head, Neck
            // and the spine, since their geometry was deleted. Inverting
            // identity yields identity, whose translation is the origin, so
            // every such lookup silently reported (0,0,0) and the pivot
            // collapsed to the model's feet.
            if (b >= PoseGlobals().size()) continue;
            const XMMATRIX global = XMLoadFloat4x4(&PoseGlobals()[b]);
            sum = XMVectorAdd(sum, global.r[3]);
            ++found;
        }
        if (found == 0) return false;
        XMStoreFloat3(&out, XMVectorScale(sum, 1.0f / static_cast<float>(found)));
        return true;
    }

    static bool CreatePaletteBuffers() {
        const size_t bones = Source().skeleton.BoneCount();
        if (bones == 0) return false;
        PaletteBytes() = static_cast<UINT>(bones * sizeof(XMFLOAT4X4));

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = PaletteBytes();
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT i = 0; i < FRAME_COUNT; ++i) {
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&Palette()[i]))))
                return false;
            D3D12_RANGE none{ 0, 0 };
            if (FAILED(Palette()[i]->Map(0, &none, &Mapped()[i]))) return false;
        }
        return true;
    }

    static std::vector<XMFLOAT4X4>& PaletteCPU() {
        static std::vector<XMFLOAT4X4> palette;
        return palette;
    }
    // Per-bone global transforms for the current frame (no inverse-bind), so a
    // joint's translation is its position in model space. Used to hang the
    // weapon off the hand.
    static std::vector<XMFLOAT4X4>& PoseGlobals() {
        static std::vector<XMFLOAT4X4> globals;
        return globals;
    }
    static int& GripBone() {
        static int bone = -1;
        return bone;
    }
    // The hand the weapon follows: the same wrist used to align the body. The
    // support hand moves independently and must not steer the rifle.
    static int& FollowBone() {
        static int bone = -1;
        return bone;
    }
    // Per-bone global transforms with the clip at rest, captured once at load.
    // WeaponFollowTransform diffs the live pose against these to get just the
    // motion the animation adds.
    static std::vector<XMFLOAT4X4>& BindGlobals() {
        static std::vector<XMFLOAT4X4> globals;
        return globals;
    }

    static void FindFollowBone() {
        FollowBone() = -1;
        const Skeleton& skeleton = Source().skeleton;
        // The hand actually on the weapon -- the same one the body is pinned by.
        // Following the opposite hand meant the rifle tracked the free hand,
        // which moves independently of the grip.
        const char* wanted = GripUsesLeftHand() ? "lefthand" : "righthand";
        for (size_t b = 0; b < skeleton.names.size(); ++b) {
            std::string name = skeleton.names[b];
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.find(wanted) == std::string::npos) continue;
            if (name.find("index") != std::string::npos ||
                name.find("middle") != std::string::npos ||
                name.find("ring") != std::string::npos ||
                name.find("pinky") != std::string::npos ||
                name.find("thumb") != std::string::npos ||
                name.find("$assimpfbx$") != std::string::npos)
                continue;
            FollowBone() = static_cast<int>(b);
            return;
        }
    }

    // Point on the weapon the aligned hand is placed at, in the gun-local space
    // this model is drawn in. The AK mesh spans z 0..1.25 from its rear, and the
    // renderer shifts it by (0, -0.10, -0.44) before scaling, so here the
    // receiver runs z -0.44..0.81 with the barrel above y ~ -0.10.
    //
    // The default targets the HANDGUARD, not the pistol grip: the hand this
    // aligns to is the forward support hand, which is open and flat and wraps
    // the barrel shroud ahead of the magazine. Aiming it at the grip pulled the
    // arm back until the rifle passed through the forearm.

    // Slide the body so the trigger hand lands on the weapon's grip.
    //
    // The offset is solved rather than guessed: the pose is already known, so
    // the right-hand bone's position under the current transform can be measured
    // and the difference from the grip applied straight back to Offset(). Doing
    // it this way means the alignment survives a change of pose, scale or asset
    // instead of being a magic number tuned against one of them.
    static void AlignHandsToWeapon() {
        if (GripBone() < 0 || PoseGlobals().empty()) return;
        if (static_cast<size_t>(GripBone()) >= PoseGlobals().size()) return;

        // Zero the offset first so the measurement reflects only the pivot,
        // rotation and scale -- otherwise each call would compound the last.
        Offset() = XMFLOAT3(0.0f, 0.0f, 0.0f);
        const XMMATRIX hand =
            XMLoadFloat4x4(&PoseGlobals()[GripBone()]) * ModelToGunLocal();
        XMFLOAT3 handPosition;
        XMStoreFloat3(&handPosition, hand.r[3]);

        const XMFLOAT3& grip = WeaponGrip();
        Offset() = XMFLOAT3(grip.x - handPosition.x,
                            grip.y - handPosition.y,
                            grip.z - handPosition.z);
    }

    // Bones whose geometry is suppressed, resolved once at load.
    static std::vector<int>& HiddenBones() {
        static std::vector<int> bones;
        return bones;
    }
    static std::vector<int>& FreeHandBones() {
        static std::vector<int> bones;
        return bones;
    }

    // Zero out the hidden bones' palette entries. A zero matrix sends every
    // vertex weighted to that bone to the origin, collapsing the triangles to
    // degenerate slivers that rasterise to nothing -- the standard trick for
    // hiding part of a skinned mesh without editing the geometry or splitting
    // the draw. Runs after ComputePalette, so the animation is untouched.
    static void CollapseHiddenBones() {
        const auto collapse = [](const std::vector<int>& bones) {
            for (int bone : bones)
                if (bone >= 0 &&
                    static_cast<size_t>(bone) < PaletteCPU().size())
                    PaletteCPU()[bone] = XMFLOAT4X4(); // all zeros
        };
        if (HideHead()) collapse(HiddenBones());
        if (HideFreeHand()) collapse(FreeHandBones());
    }

    static void FindHiddenBones() {
        HiddenBones().clear();
        FreeHandBones().clear();
        const Skeleton& skeleton = Source().skeleton;
        const char* freeWristName =
            GripUsesLeftHand() ? "righthand" : "lefthand";
        int freeWrist = -1;
        for (size_t b = 0; b < skeleton.names.size(); ++b) {
            std::string name = skeleton.names[b];
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // Head and everything parented under it. The neck stays: it is
            // mostly hidden by the collar and keeps the shoulders from ending
            // in a hole.
            if (name.find("head") != std::string::npos)
                HiddenBones().push_back(static_cast<int>(b));

            const size_t colon = name.find_last_of(':');
            const std::string leaf =
                colon == std::string::npos ? name : name.substr(colon + 1);
            if (leaf == freeWristName)
                freeWrist = static_cast<int>(b);
        }

        // Include wrist and every descendant, which covers all five fingers
        // without depending on exporter-specific finger naming.
        if (freeWrist >= 0) {
            for (size_t b = 0; b < skeleton.parent.size(); ++b) {
                int ancestor = static_cast<int>(b);
                while (ancestor >= 0) {
                    if (ancestor == freeWrist) {
                        FreeHandBones().push_back(static_cast<int>(b));
                        break;
                    }
                    if (static_cast<size_t>(ancestor) >= skeleton.parent.size())
                        break;
                    ancestor = skeleton.parent[ancestor];
                }
            }
        }
    }

    // The bone the weapon is held by. Matched once at load.
    static void FindGripBone() {
        GripBone() = -1;
        const Skeleton& skeleton = Source().skeleton;
        const char* wanted = GripUsesLeftHand() ? "lefthand" : "righthand";
        for (size_t b = 0; b < skeleton.names.size(); ++b) {
            std::string name = skeleton.names[b];
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            // The wrist itself, not a finger and not one of Assimp's synthetic
            // transform nodes.
            if (name.find(wanted) == std::string::npos) continue;
            if (name.find("index") != std::string::npos ||
                name.find("middle") != std::string::npos ||
                name.find("ring") != std::string::npos ||
                name.find("pinky") != std::string::npos ||
                name.find("thumb") != std::string::npos ||
                name.find("$assimpfbx$") != std::string::npos)
                continue;
            GripBone() = static_cast<int>(b);
            return;
        }
    }
    static std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FRAME_COUNT>& Palette() {
        static std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, FRAME_COUNT> palette;
        return palette;
    }
    static std::array<void*, FRAME_COUNT>& Mapped() {
        static std::array<void*, FRAME_COUNT> mapped{};
        return mapped;
    }
    static UINT& PaletteBytes() {
        static UINT bytes = 0;
        return bytes;
    }

    // Extents of the mesh in its own space.
    static inline XMFLOAT3 s_lo{};
    static inline XMFLOAT3 s_hi{};
    // Unskinned primitives discarded at load, for the load log.
    static inline size_t s_droppedPrimitives = 0;
};
