# Optional sculpt auto-merge update

Enable **Auto-merge sculpt strokes** in the Level Editor's terrain tools.
The checkbox defaults to off and is a session preference. **Bake to One Stamp**
continues to provide manual whole-level baking for occasional stamping.

After a completed stroke reaches the normal visual sync, auto-merge folds
32 or more consecutive recent stamps into a local 512 by 512 heightmap.
The region, including padding, is capped at 64 metres across. A distant edit,
a larger footprint, or a whole-level baked stamp stops the group. Merging
preserves operation order, including flatten and replace stamps, by sampling
the completed surface. Distant stamps are retained; the whole island is never
rebaked automatically. If the group is too small or the active textures leave
no atlas capacity, it remains as authored.

The merge belongs to the same undo action as the stroke. Each merge writes a
new `HM_Merged_*.png` into the map's folder, so future merges cannot overwrite
an image referenced by undo or redo. Images remain on disk after history is
cleared. Save retains the merged stamp references; undo history is session-only.
Turning the checkbox off stops future merges and retains completed edits.

The existing frame-local atlas buffers upload the changed image region.
Inactive texture layers can be reused after a merged image has been loaded;
an undo reloads its referenced images. No new GPU pass, resource allocation,
queue scheduling, or synchronization is introduced.

This is a worthwhile optional improvement, not an urgent fix. GPU performance
gains have not been measured. The editor reports the CPU time of each merge.
Resampling can soften fine detail, and the replacement image captures the
procedural ground as it was at the time of the merge. Visual quality and GPU
timings need in-app validation before considering a default-on change.
