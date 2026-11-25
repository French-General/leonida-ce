<h1 align="center">
  <br>
  <a><img src="assets/logo.svg" alt="Leonida CE" width="200"></a>
  <br>
  <b>Leonida CE</b>
  <br>
</h1>

<h4 align="center">Grand Theft Auto VI (Xbox Series) running natively on Windows, built on your PC from your own copy.</h4>

<p align="center">
  <a href="#status">Status</a> |
  <a href="#how-to-build">How to build</a> |
  <a href="#how-it-works">How it works</a> |
  <a href="#want-to-help">Want to help?</a> |
  <a href="#credits">Credits</a> |
  <a href="docs/i18n/README.md">Other languages</a>
</p>

**Leonida CE** (community edition) is a static recompilation of GTA VI for Xbox Series. The console's CPU is x86-64
like a PC's, so the game's own code runs natively; the console's system software is reimplemented, and its RDNA2
GPU programs are translated to SPIR-V for Vulkan. It continues the original Leonida project, which went private;
the history, the GPLv3 license and the credit to everyone who worked on it are kept.

> [!IMPORTANT]
> This repository has **no game files, keys or executables**, and doesn't explain how to get them. You need your own
> copy with its files readable on disk; nothing here decrypts or bypasses any protection. Everything the build makes
> comes from your copy and stays on your PC. **Never share it.**

## Status

**Not playable yet. The game boots and runs, but the screen is still black.**

| | |
|---|---|
| Boots the real game | ✅ On native Windows (and Wine): runs 10+ minutes without a crash, starts its boot state machine, loads saves and the profile, streams speech, submits tens of thousands of draws |
| A picture on screen | ⚠️ Every frame is presented into the game window, but black: the final UI composite draw writes no pixels yet. This is the current blocker |
| Shaders (offline) | ✅ All 26,503 translated to SPIR-V and validated |
| Shaders (created at run time) | ⚠️ 1,536 of 2,675 translated; the rest run as empty placeholders |
| Models, textures, audio | ✅ Exported (382,781 models; DDS textures; Ogg Opus / WAV audio) |
| Playable | ❌ Not yet |

Many of the fixes that got it this far are deliberate stand-ins ("hacks"), each behind an environment switch and
logged in [hacks/BOOT_NOTES.md](hacks/BOOT_NOTES.md). The full status table is in [docs/status.md](docs/status.md).

## How to build

It isn't downloaded ready-made: it's built on your PC from your copy (Xbox Series package **1.0.2.1**).

1. Install the prerequisites: Windows 10/11 x64 with ~400 GB free (~30 GB without the asset export), Python 3.12+,
   .NET 8 SDK, the Vulkan SDK, LLVM 21+ with the AMDGPU target (`llvm-mc`), and Visual Studio 2022 with C++ and the
   Windows SDK 10.0.26100+. ffmpeg is optional. Details and shortcuts: [docs/building.md](docs/building.md#prerequisites).
2. `git clone --recursive https://github.com/IAmNotSiob/leonida-ce` (a ZIP download misses the submodule).
3. Double-click **`Build.bat`**.

It finds your copy by itself, checks everything is installed, and builds into `Documents\Leonida`. A stopped build
resumes where it left off. Prefer a terminal? `python -I make_game.py --help`. Every option, low-disk builds, Linux,
Wine/Proton and macOS are in [docs/building.md](docs/building.md).

## How it works

| Piece | What it does | Where |
|---|---|---|
| Version check | Checks every file of your copy against the supported version | `game/`, `make_game.py` |
| Shaders | Carves the console's GPU programs from `shaders.rpf`, disassembles them with LLVM and translates them to SPIR-V | `tools/rdna2spirv.py` and friends |
| Assets | Exports models, textures and audio from the RPF9 archives | `tools/rpf9dump` |
| Runtime | Loads the game's executable and answers its calls to the console's system software (GDK, XMem, Xbox D3D12 on Vulkan) | `runtime/` |
| Boot hacks | Stand-ins that get the real game further, each behind an env switch | `runtime/shims/`, [hacks/BOOT_NOTES.md](hacks/BOOT_NOTES.md) |

Design notes: [docs/design.md](docs/design.md) (translator contract), [docs/d3d12_vulkan.md](docs/d3d12_vulkan.md)
(Xbox D3D12 on Vulkan), and the index in [docs/README.md](docs/README.md).

## Want to help?

The open problems, most important first:

- **The black screen.** The UI composite draw (`sga_dr_vs_pos_color_tex` + `default_ps_tex`, topology 17) writes
  nothing into the back buffer. GPU readback tooling is built in (`LEONIDA_CE_RTDIAG`).
- **The `0xF800xxxx` instruction.** About 500 run-time compute shaders stop on a 4-byte word that llvm-mc decodes as
  an 8-byte export. It looks like an Xbox-only scalar memory op; nobody knows its semantics yet.
- **Topology 17 (RECTLIST)** still has to be expanded into two triangles.

Read [CONTRIBUTING.md](CONTRIBUTING.md) first. **Never attach game files, extracted assets or generated shaders**
to issues or pull requests.

## Credits

- [rpf9-explorer](https://github.com/breadbbbb/rpf9-explorer) by breadbbbb: the RPF9 model, texture-pool and AWC
  decoders used by `tools/rpf9dump` (git submodule).
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT): GFX10 resource-descriptor layout (`tools/gfx10_formats.json`),
  and the ACO/RADV rules used as reference for 16-bit results and shader system registers.
- [libHttpClient](https://github.com/microsoft/libHttpClient) (MIT) by Microsoft: the XTaskQueue/XAsync implementation
  in `third_party/xtaskqueue`, used by the offline `xgameruntime` shim.
- [LLVM](https://llvm.org/): AMDGPU disassembler.
- Logo: ["Grand Theft Auto VI" icon by SlyDevil](https://www.steamgriddb.com/icon/122618) on SteamGridDB, with a "CE" mark added for Leonida CE.
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools): assembly and validation.

Grand Theft Auto and all related names are trademarks of their owners. This project is not affiliated with or
endorsed by them.

## License

GPLv3 (or any later version). See [LICENSE](LICENSE). Third-party code keeps its own license.
