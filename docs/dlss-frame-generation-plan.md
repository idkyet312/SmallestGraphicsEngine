# DLSS Frame Generation and NVIDIA Reflex

Extend the existing Streamline 2.14.1 integration; both new settings default
to off. Reflex offers Off, On, and On + Boost independently of DLSS SR.
Frame Generation initially generates one extra frame (2x) on the visibility
buffer DLSS path, with Reflex automatically enabled while FG is active.
Use SR/DLAA or upscaling RR. Native mid-frame RR is excluded because its
depth/motion guides precede forward rendering and depth replay.

## Presentation changes requiring approval under AGENTS.md

NVIDIA requires the DLSS-G plugin to be loaded before swap-chain creation.
Loading it while FG is off still introduces an off-screen copy and an extra
presentation queue. Therefore keep it unloaded by default, and recreate the
swap chain when the user changes the FG setting. At that rare transition:

1. Disable interpolation and clear old input tags.
2. Drain existing engine queues using the existing GPU-idle helper.
3. Release swap-chain buffers and the swap chain.
4. Load/unload DLSS-G, recreate the swap chain through the existing proxy
   DXGI factory, and recreate RTVs in their existing descriptor slots.
5. Create/release display-sized, per-frame-slot HUD-less and UI textures.

Depth/motion inputs are snapshot-tagged on the presenting graphics queue,
because the engine reuses depth and its asynchronous compute queue can
overwrite motion. HUD-less/UI resources remain valid through Present.
Use the SDK's default queue blocking mode; introduce no custom cross-queue
fences or hot-path GPU idle waits. Disable interpolation before resize,
window transitions, loading, pause, diagnostics, or missing valid inputs.

## Frame integration

Use one Streamline frame token for simulation, DLSS/RR constants, render
submission and Present. Call Reflex sleep before consuming input, then mark
simulation, render submission and Present boundaries via the PCL plugin.
Supply PCL ping and left-click flash markers.

Tag depth/motion where DLSS consumes them. Copy the final tone-mapped scene
before ImGui into the HUD-less texture. Render ImGui once into a transparent
UI texture and composite it onto the back buffer with premultiplied alpha;
tag both textures before Present. Keep the normal ImGui path when FG is off.

## Verification

Build Release; run CPU tests, including setting defaults, clamping and
persistence. Validate runtime support/fallback and FG status on the installed
adapter. Use the visibility parity harness with FG off; validate HUD stability,
camera motion, resize and repeated FG toggles on FG-capable hardware.
CPU tests cannot establish generated-frame quality or latency improvements.

References: the SDK's ProgrammingGuideDLSS_G.md, ProgrammingGuideReflex.md,
ProgrammingGuidePCL.md and ProgrammingGuideManualHooking.md.
