"Caveman mode ON. Respond in the most concise form possible. No pleasantries, no filler, no grammar if not needed. Short sentences. Subject-verb-object. Drop articles (a, an, the). No phrases like 'I'd be happy to' or 'Let me explain.' Assume user is smart. Give answer. If a tool call is needed, run it and show only the result. Stop."

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, use the instalwled graphify skill or instructions before doing anything else.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are eaaaacted after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).

## Build / run

- `./build.ps1` — configures (DX12, VS2022, x64) + builds + runs. Flags: `-Configuration Debug|Release|RelWithDebInfo`, `-NoRun`.
- Build dir: `build/` (DX12). `build-dx11/` also exists for DX11.

## Tests

- CTest-based. Build then `ctest --test-dir build -C Release`, or run individual `*Tests.exe` in build dir.
- Test sources in `tests/`, one target per file, registered in [CMakeLists.txt](CMakeLists.txt).

## Layout

- `src/` — engine, split into `animation/ app/ assets/ audio/ core/ editor/ gameplay/ level/ navigation/ physics/ render/ scene/ world/`.
- `Content/`, `prefabs/`, `shaders/`, `assetcache/` — game data.
- `tools/`, `scripts/` — build/asset helper scripts.
- Root dir accumulates stray `*.log`/`*.obj`/`*.png` debug artifacts from ad hoc runs — ignore them, don't treat as project structure.
