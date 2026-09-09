#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void BindSniperScopeMaterial() {
    const auto& aperture = GunModel::R700ScopeAperture();
    if (!g_sniperScope.HasRenderedOnce() || !aperture) return;
    for (auto& primitive : aperture->primitives) {
        auto& material = primitive.material;
        if (!material || material->baseColorTexture.Get() == g_sniperScope.Texture())
            continue;
        material->baseColorTexture = g_sniperScope.Texture();
        material->InvalidateTextureBindings();
    }
}

// The scope uses its own view storage and a square HDR feed. Presentation
// effects and temporal history belong to the main camera.
static void RenderSniperScopeTexture(float now) {
    scene.sniperScopeFeedReady = false;
    const bool frameReady = PrepareR700ScopeFrame(scene);
    if (!g_sniperScope.ShouldRender(
            frameReady && scene.sniperPictureInPicture && scene.sniperScopeActive &&
            IsSceneScreen() && !g_game.loading.Active())) return;

    const Camera savedCamera = scene.camera;
    const XMFLOAT2 savedJitter = scene.temporalJitterPixels;
    const float savedWidth = scene.renderWidthOverride;
    const float savedHeight = scene.renderHeightOverride;
    const bool savedScopePass = scene.sniperScopeCameraPass;
    const bool savedDrawViewmodel = scene.drawViewmodel;

    // Keep sky and lighting position consistent with the explicit scope view.
    const XMMATRIX scopeBasis = XMMatrixInverse(nullptr, scene.sniperScopeView);
    XMStoreFloat3(&scene.camera.Position, scopeBasis.r[3]);
    XMStoreFloat3(&scene.camera.Front, scopeBasis.r[2]);
    XMStoreFloat3(&scene.camera.Up, scopeBasis.r[1]);
    scene.sniperScopeCameraPass = true;
    scene.drawViewmodel = false;
    scene.renderWidthOverride = static_cast<float>(g_sniperScope.Width());
    scene.renderHeightOverride = static_cast<float>(g_sniperScope.Height());
    scene.temporalJitterPixels = XMFLOAT2(0.0f, 0.0f);

    // Re-base the per-frame upload arenas onto the scope's slice. Without this
    // the scope's draws consume -- and overwrite -- the main view's matrix and
    // instance records while the main view is still recording its own.
    mainShader.BeginView(1);
    g_meshShader.BeginView(1);

    // The scope renders through the same visibility buffer the main view uses,
    // pointed at the scope's own depth buffer, viewport and target by
    // BindViewSurface. One instance rather than two: a second would duplicate
    // the fixed-size CPU geometry pools (tens of MB, independent of the scope's
    // resolution), recompile every PSO, and interleave two claims on the one
    // bindless transient ring, whose per-frame reset does not expect them.
    //
    // Bound before anything that could recreate the visibility depth snapshot,
    // which takes its dimensions from ActiveDepthBuffer().
    const bool scopeUsesVisibilityBuffer =
        scene.useVisibilityBuffer && visBuffer.initialized && visBuffer.scopeView;
    if (scopeUsesVisibilityBuffer) {
        VisibilityBufferDX12::ViewSurface surface;
        surface.depthBuffer = g_sniperScope.Depth();
        surface.depthDSV = g_sniperScope.DSV();
        surface.hasDepthDSV = true;
        surface.viewport = &g_sniperScope.Viewport();
        surface.scissor = &g_sniperScope.Scissor();
        surface.destination = g_sniperScope.Texture();
        surface.viewWidth = g_sniperScope.Width();
        surface.viewHeight = g_sniperScope.Height();
        surface.isScope = true;
        visBuffer.BindViewSurface(surface);
    }

    g_sniperScope.Begin(g_dx12.commandList.Get());
    {
        ProfilerDX12::Scope profile(
            g_profiler, "Scope/Forward", g_dx12.commandList.Get());
        // No MSAA either way. HDR follows the path: the visibility buffer works
        // in linear radiance and the lens keeps it that way for the main view's
        // tone map, while the Forward fallback writes straight into the lens
        // texture and has to tone map as it goes.
        const bool scopeHDR = true;
        mainShader.SetMSAAEnabled(false);
        mainShader.SetHDRTargetEnabled(scopeHDR);
        mainShader.SetExtensionMotionEnabled(false);
        g_meshShader.SetMSAAEnabled(false);
        g_meshShader.SetHDRTargetEnabled(scopeHDR);
        g_meshShader.SetExtensionMotionEnabled(false);
        g_terrain.SetMSAAEnabled(false);
        g_terrain.SetHDRTargetEnabled(scopeHDR);
        skyRenderer.SetMSAAEnabled(false);
        skyRenderer.SetHDRTargetEnabled(scopeHDR);
        // The sun disc and its lens artefacts are a main-camera presentation
        // effect. Through a telescopic sight they would be drawn at the scope's
        // magnification and dominate the lens.
        skyRenderer.SetSunLens(false, scene.lightColor,
            scene.sunAngularRadiusDegrees, scene.sunDiscIntensity,
            scene.sunHaloIntensity);
        // The same bracket the main view puts around its own sky, and not
        // optional here -- for the reason written on BeginHDRBackground
        // itself. The resolve compute shader leaves outputColor untouched
        // wherever the visibility buffer holds no geometry, on the contract
        // that a cleared target already carries this view's sky.
        //
        // The scope used to draw its sky into g_sniperScope's own render
        // target, which the CopyResource below then overwrote, so it satisfied
        // neither half of that contract: no clear, and no sky in the surface
        // the resolve preserves. Every background pixel of the lens therefore
        // kept whatever the MAIN view left in outputTexture last frame --
        // including its forward-extensions pass, which is where the viewmodel
        // is drawn. The player saw their own rifle and hands through the
        // scope, one frame stale, while the scope's own sky was discarded.
        //
        // Bracketing here fixes both halves at once: the clear removes the
        // stale main-view image, and the sky lands in the surface that
        // CopyResolveOutputTo actually reads. It must follow BindViewSurface
        // above, because the clear and the draw take the scope's viewport
        // through ActiveViewport().
        if (scopeUsesVisibilityBuffer)
            visBuffer.BeginHDRBackground(g_dx12.commandList.Get());
        skyRenderer.Render(
            scene.camera, scene.EffectiveCameraFOV(), scene.lightPos, now,
            scene.enablePhysicalAtmosphere, false,
            XMFLOAT4(scene.atmosphereRayleighStrength,
                     scene.atmosphereMieStrength,
                     scene.atmosphereMieAnisotropy,
                     scene.atmosphereAerialDensity),
            XMFLOAT4(0.0f, 0.0f, scene.atmosphereCloudBaseHeight,
                     scene.atmosphereCloudThickness),
            1, 1.0f);
        if (scopeUsesVisibilityBuffer)
            visBuffer.EndHDRBackground(g_dx12.commandList.Get());

        // World geometry, terrain, foliage and grass, lit by the shadow map
        // the previous frame produced for the main view. The scope frustum is
        // a subset of that view, so the map covers it.
        if (scopeUsesVisibilityBuffer) {
            // View slot 1. RenderVBDraw keeps its culling context and compacted
            // indirect stream per slot, so the scope cannot overwrite the main
            // view's before the GPU has consumed it, and its isScopeView branch
            // leaves the main view's terrain and destruction ownership flags
            // alone -- those describe a decision made for the other frustum.
            //
            // No HZB: occlusion history belongs to the main camera, and the
            // scope's frustum would reproject against it wrongly.
            RenderVBDraw(scene, mainShader, visBuffer, geo, packed,
                g_scopeShadowLightSpace, g_scopeShadowResource,
                nullptr, false, XMMatrixIdentity(), floorMaterial,
                (!g_emptyLevelMode && g_showH2Model) ? crateModel : nullptr,
                /*viewSlot=*/1);
        } else {
            RenderForward(scene, mainShader, geo, g_prefabRenderBatches,
                crateModel, floorMaterial,
                g_scopeShadowLightSpace, g_scopeShadowResource,
                false, true, false);
        }

        if (scopeUsesVisibilityBuffer) {
            visBuffer.BeginForwardExtensions(g_dx12.commandList.Get());
            const bool mainTerrainOwned = g_terrainInVisibilityBuffer;
            g_terrainInVisibilityBuffer = visBuffer.terrainVisibilityActiveThisFrame;
            RenderForward(scene, mainShader, geo, g_prefabRenderBatches,
                crateModel, floorMaterial, g_scopeShadowLightSpace,
                g_scopeShadowResource, true, true, false);
            g_terrainInVisibilityBuffer = mainTerrainOwned;
        }

        // Skinned actors live outside RenderForward in the primary path.
        // Enemies are the whole point of a magnified sight, so they are drawn
        // here with their weapons, exactly as the main view draws them.
        if (!g_emptyLevelMode && g_banditLoaded) {
            ProfilerDX12::Scope banditProfile(
                g_profiler, "Scope/Bandits", g_dx12.commandList.Get());
            const XMMATRIX view = scene.GetViewMatrix();
            const XMMATRIX projection = scene.GetUnjitteredProjectionMatrix();
            for (auto& bandit : g_bandits) {
                if (!bandit) continue;
                bandit->Draw(mainShader, view, projection,
                             g_scopeShadowLightSpace);
                if (bandit->HasGunPose() && GunModel::Loaded()) {
                    mainShader.Use(false);
                    DrawMeshAt(GunModel::Mesh(), mainShader,
                        bandit->GunWorldMatrix(), view, projection,
                        g_scopeShadowLightSpace, true);
                }
            }
        }

        // Water, in the same place the main view draws it: inside the forward
        // extensions bracket, after the opaque scene. The pass copies its own
        // render target to refract and reflect through, so the sky and the
        // resolved terrain have to be in it already -- they are, which is why
        // this sits at the end of the bracket rather than anywhere earlier.
        //
        // This must stay LAST in the bracket. Render() leaves colour bound
        // with no depth-stencil view, so anything added after it has to rebind
        // for itself.
        // Matches the main view's waterPassEnabled, minus its raytracing
        // exclusion: scopeUsesVisibilityBuffer already implies the visibility
        // path, so the ray-traced path never reaches here.
        if (scopeUsesVisibilityBuffer && waterRenderer.initialized &&
            !g_emptyLevelMode &&
            (g_ocean.IsInitialized() || g_water.IsInitialized()) &&
            !(DeploymentPlanningActive() && g_deploymentDebugHideWater)) {
            ProfilerDX12::Scope waterProfile(
                g_profiler, "Scope/Water", g_dx12.commandList.Get());
            WaterRendererDX12::ViewOverride scopeView;
            scopeView.width = g_sniperScope.Width();
            scopeView.height = g_sniperScope.Height();
            scopeView.viewport = g_sniperScope.Viewport();
            scopeView.scissor = g_sniperScope.Scissor();
            scopeView.secondaryView = true;
            // The target is the visibility buffer's scope-sized output, not
            // the lens texture: the resolve copy below reads that, so water
            // drawn straight into the lens would be overwritten by it.
            waterRenderer.Render(
                scene, g_ocean, g_water,
                visBuffer.GetOutputResource(), visBuffer.GetOutputRTV(),
                g_sniperScope.Depth(), g_specularEnvironmentResource,
                /*hdrTarget=*/true,
                /*motionTarget=*/nullptr, D3D12_CPU_DESCRIPTOR_HANDLE{},
                D3D12_RESOURCE_STATE_DEPTH_WRITE,
                /*profiler=*/nullptr, &scopeView);
        }
    }
    if (scopeUsesVisibilityBuffer) {
        visBuffer.EndForwardExtensions(g_dx12.commandList.Get());
        ProfilerDX12::Scope profile(
            g_profiler, "Scope/Resolve Copy", g_dx12.commandList.Get());
        // The scope takes the visibility buffer's resolve output and stops
        // there. Its post chain is NOT run for the lens.
        //
        // Post is a main-camera presentation layer: the lens flare and lens
        // dirt describe artefacts in the player's own optics, and running them
        // for a telescopic sight drew them at scope magnification, filling the
        // glass with concentric rings centred on the lens -- the same reason
        // the sun disc is disabled for this pass above. Exposure has the same
        // problem in slower form: a second histogram adapting to the magnified
        // framing would drift away from the main view until the lens and the
        // world disagreed about how bright the same wall is.
        //
        // So the lens receives linear radiance and the main view's single
        // tone-map covers it too, exactly as it did on the Forward path. What
        // the visibility buffer adds here is the lighting and material work up
        // to the resolve, which is what made the lens disagree with the world.
        //
        // The lens texture is still the render target Begin() bound, so hand it
        // over as a copy destination first.
        g_sniperScope.TransitionForCopyDestination(g_dx12.commandList.Get());
        visBuffer.CopyResolveOutputTo(g_dx12.commandList.Get(),
                                      g_sniperScope.Texture(),
                                      D3D12_RESOURCE_STATE_COPY_DEST);
        g_sniperScope.TransitionToShaderResource(
            g_dx12.commandList.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        visBuffer.UnbindViewSurface();
    } else {
        g_sniperScope.End(g_dx12.commandList.Get());
    }
    // Only bound once a real image exists: a freshly created default-heap
    // texture holds undefined contents, and the rifle is visible at the hip
    // long before the first scope frame is drawn.
    BindSniperScopeMaterial();
    scene.sniperScopeFeedReady = true;

    if (GetEnvironmentVariableA("SGE_R700_TEST", nullptr, 0) > 0) {
        static UINT scopeTestFrames = 0;
        if ((scopeTestFrames++ % 120) == 0)
            std::ofstream("r700_scope_smoke.log", std::ios::app)
                << "frame=" << scopeTestFrames << " weapon=" << GunModel::SelectedWeapon()
                << " vb=" << scopeUsesVisibilityBuffer << " ready=" << scene.sniperScopeFeedReady
                << " size=" << g_sniperScope.Width() << 'x' << g_sniperScope.Height()
                << " fov=" << scene.sniperScopeFOV << " aperture=" << g_r700ScopeFrame.halfTangent
                << " ads=" << scene.adsBlend << '\n';
    }

    // Restore the main view's arena slice and viewport before the primary
    // camera's own recording resumes.
    mainShader.BeginView(0);
    g_meshShader.BeginView(0);
    g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
    g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);

    scene.camera = savedCamera;
    scene.temporalJitterPixels = savedJitter;
    scene.renderWidthOverride = savedWidth;
    scene.renderHeightOverride = savedHeight;
    scene.sniperScopeCameraPass = savedScopePass;
    scene.drawViewmodel = savedDrawViewmodel;
}
