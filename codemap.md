# Leonida/

Recompilación estática de GTA VI Xbox Series a PC Windows x64: el código x86-64 corre nativo, el system software se reimplementa (shims), los shaders RDNA2 precompilados se traducen a SPIR-V Vulkan, y modelos/texturas/audio se exportan desde RPF9. Todo se construye en tu PC desde tu propia copia (versión 1.0.2.1); el repo no contiene juego, claves ni ejecutable.

## Responsibility

Orquestar el build offline por etapas idempotentes (`make_game.py` + `Build.bat`) que verifican la copia, traducen 26.503 shaders a SPIR-V validado, exportan assets y compilan el loader `gta6rt.exe`. Estado: temprano, no bootea; lo que no necesita el exe está hecho.

## Design

- **Orquestador monolítico:** `make_game.py` (430 líneas, `python -I`, sin dependencias externas) con `STAGES = [check, shaders, assets, runtime, pipelines]`; cada stage salta trabajo hecho (existe `index.json`/`manifest.json`/binario) para continuar builds parados.
- **Pipeline por contratos JSON:** cada herramienta escribe `index.json`/`manifest.json`/`fssm_index.json` que la siguiente consume sin releer el `.rpf` (ver `tools/codemap.md`).
- **Carve sin TOC:** RPF9 tiene tabla cifrada; todo es carving (`mmap` + regex/zlib/entropía), nunca descifrado.
- **Registro estático + fail-soft:** shims se auto-registran vía `SHIM_TABLE` en init de estáticos; lo desconocido cae en thunks JIT `unimplemented` que devuelven 0 y loguean (ver `runtime/shims/codemap.md`).
- **RAII a medias (deuda conocida):** `rt::Loader` libera con `VirtualFree` manual y `Module*` con `new` crudo; migrar a `unique_ptr` + wrapper `VirtualAlloc` (ver `runtime/src/codemap.md`).

## Flow

1. `Build.bat` → `py -I make_game.py` → `find_dumps()`/`choose_game()` localiza tu copia → `check()` valida 108 ficheros contra `game/xbox-1.0.2.1.json`.
2. `shaders()`: `shaders.rpf` → carve DXBC → unpack XBCH→FSSM → `llvm-mc gfx1013` → `rdna2spirv` → `spirv-as/val` → `<work>/shaders/spirv/index.json`.
3. `assets()`: compila `rpf9dump` (dotnet) → `models`/`textures` por `MODEL_ARCHIVES` → `ttpf` por cada `.ttpf` → `audio`/`xma` sobre `audio.rpf` → `awc_opus.py` → Ogg.
4. `runtime()`: solo Windows; `scripts/build_runtime.ps1` (CMake+Ninja o `cl.exe`) → `runtime/build/gta6rt.exe` → copia a `<out>/` + `game_path.txt`.
5. `pipelines()`: `vk_pipeline_check.exe` crea cada shader module/pipeline en tu GPU y guarda `pipeline_cache.bin`.

## Integration

- **Dependencias externas:** Python 3.11+, .NET 8 SDK, Vulkan SDK (`spirv-as`/`spirv-val`), LLVM 21+ (`llvm-mc` AMDGPU), VS2022 + C++ CMake tools, `ffmpeg` opcional (XMA2→WAV).
- **Invariantes:** lee ficheros planos, no descifra ni evade protección; nada generado sale del PC (`--out` fuera de `--game`); `third_party/rpf9-explorer` es submódulo (excluido del codemap).
- **Contrato runtime:** user SGPRs, push constants, memoria/bibli bindless, vertex fetch y system values en `docs/design.md` (excluido del codemap, leer para GPU).

## Repository Directory Map

| Directory | Responsibility | Link |
|-----------|----------------|------|
| `game/` | Version pinning: manifiesto `xbox-1.0.2.1.json` (108 ficheros size+sha256), puerta `check` | [View Map](game/codemap.md) |
| `tools/` | Extracción/traducción offline: shaders RDNA2→SPIR-V, carve forense, texturas, audio, inventario | [View Map](tools/codemap.md) |
| `tools/rpf9dump/` | Export bulk C# sobre `RpfPreview.Core`: OBJ/DDS/AWC/XMA en 5 modos CLI | [View Map](tools/rpf9dump/codemap.md) |
| `runtime/` | Loader Windows x64 `gta6rt` + validación GPU `vk_pipeline_check` (CMake, solo Windows) | [View Map](runtime/codemap.md) |
| `runtime/src/` | Entrada `main --dry-run` + `pe_loader` que mapea/relocaliza/enlaza sin ejecutar | [View Map](runtime/src/codemap.md) |
| `runtime/shims/` | Compat Xbox→host (GDK/XMem/`d3d12_xs`); esqueleto, D3D12→Vulkan pendiente | [View Map](runtime/shims/codemap.md) |
| `scripts/` | Helpers PowerShell solo-Windows: build, first-look, carve_all, meta_to_xml | [View Map](scripts/codemap.md) |
| `viewer/` | Visor local `world.json`+`.glb` (http.server + Three.js CDN), fuera del build | [View Map](viewer/codemap.md) |

Root assets: `make_game.py` (orquestador), `Build.bat` (doble-click → `make_game.py`), `README.md`/`CONTRIBUTING.md` (uso/estado), `docs/design.md` (contrato GPU), `assets/logo.png`, `.slim/codemap.json` (estado change-detection). Excluidos del codemap por política: `docs/**`, `*.md` (salvo este atlas), `runtime/test/**`, `runtime/build/**`, `third_party/**`, `build/`/`dist/`.
