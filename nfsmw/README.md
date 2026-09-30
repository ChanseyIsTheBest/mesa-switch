# mesa-switch 26.2.2 with the nfsmw-nx driver changes

This tree is danfromtico/mesa-switch as uploaded (Mesa 26.2.2 with the Horizon backend) plus the driver changes
of nfsmw-nx (`mesa/mesa-switch-nfsmw.patch`, written against commit `1a8c1a66d6f`, Mesa 26.2.1). Nothing in it has
been compiled yet: the port was checked by review (below), and the first build on your machine is the real test.

`mesa-switch-nfsmw-26.2.2.patch` in this folder is the whole port as a diff against the uploaded tree (20 files),
for applying it to a git checkout instead of using this folder.

## What the port contains

| Change | Files | Default |
|---|---|---|
| ZCULL (hierarchical depth culling) on the Vulkan path: ZCULL geometry from the kernel, a ZCULL context bound to each 3D channel, a ZCULL plane for eligible depth images | `nouveau_horizon.h`, `nouveau_horizon_device.c`, `nvkmd_switch_pdev.c`, `nvkmd_switch_dev.c`, `nvk_image.c` | on for depth images without `TRANSFER_DST` |
| NAK scheduling: texture and global-memory latency 32 → 200 cycles | `compiler/nak/opt_instr_sched_common.rs` | on |
| NAK branch flattening: NIR `peephole_select` limit 0 → 8 | `compiler/nak_nir.c` | on |
| NAK SM50 fix: `FADD` with a long immediate and `.SAT` no longer becomes `FADD32I` (which drops the saturation on Maxwell) | `compiler/nak/sm50.rs` | on |
| Shader cache key includes a revision of these compiler changes | `nvk_shader.c` | on |
| Fragment subtiling split of deko3d for the T210 | `nvk_shader.c` | on; `NVK_SUBTILING_KNOB` overrides |
| Draw path: cheaper emission, fewer constant-buffer rebinds, dynamic-state shortcuts, pipeline prefetch, each with a self-check | `nvk_cmd_draw.c`, `nvk_cmd_buffer.c/h`, `vk_pipeline.c/h` | on; `NVK_SWITCH_DIBUJO=0` turns all off; the app can request each |
| Dynamic uniform buffers written to the root table by differences, with a self-check | `nvk_cmd_buffer.c/h`, `nvk_cmd_draw.c` | on; `NVK_SWITCH_DYN_UBO_DELTA=0` turns it off |
| Buffer copies on the copy engine | `nvk_cmd_meta.c`, `nvk_device.c` | off; `NVK_COPY_ENGINE=1` |
| Build: bindgen treats `__sFILE` as opaque and skips layout tests; rustc wrappers find whichever MSYS2 gcc exists; LLVM `demangle` component; `-Doptimization=2` in the Docker build | `bindgen-switch-wrapper.sh`, `rustc-*-wrapper.sh`, `meson.build`, `build-switch.sh` | |

The shared structures `nvk_switch_set4` and `nvk_switch_dibujo` (read and written by the app through weak symbols)
are unchanged from nfsmw-nx; UnleashedRecomp's declarations match them (584-byte `nvk_switch_dibujo`, 16 parts,
13 counters, 5 improvements).

## How it was ported

The nfsmw-nx patch does not apply to this newer tree with `git apply` (20 files fail on context). The 35 files it
touches were taken at `1a8c1a66d6fd8d65f10107c4627ffc3606ba5631` and merged three ways (nfsmw-nx, base, this tree):

- 13 files: the nfsmw-nx changes are already upstream here (fixes it had carried from later mesa-switch commits:
  memory arena, descriptor tables, query pools, queue, heap, command pool, memory stream, descriptor set, device,
  `nouveau_horizon_gm20b.c`). Nothing to do.
- 19 files merged without conflict.
- 3 conflicts, resolved:
  - `nvk_cmd_buffer.h`: both sides added declarations at the same place; both kept.
  - `nvk_shader.c`: upstream raised the compiler ABI revision to 9; nfsmw-nx's NAK-fix revision bit is kept on top,
    so no old binary is reused.
  - `nvk_nir_lower_descriptors.c`: upstream's version kept. It is a newer form of the same GM20B fix nfsmw-nx
    carried (UBO descriptors in constant banks where a global load would hang the GPU), limited to where it is
    needed. `NVK_SWITCH_NO_UBO_CBUF` still works.
- `build-unified.sh` keeps upstream's host-tool setup: nfsmw-nx's `-Dshared-llvm=enabled` was for its MINGW64
  environment, while this tree's MSYS2 files target CLANG64/CLANGARM64.

Checked by review: every function, field and macro the ZCULL code uses exists in this tree; upstream's MME fix
(flush the deferred Horizon sync before `CALL_MME_MACRO(NVK_MME_BIND_CBUF_DESC)`) is on the only constant-buffer
binding path, so no nfsmw-nx fast path bypasses it; no identifier used by the added code was renamed or removed
upstream; the dynamic-UBO delta path is generic (any descriptor set with dynamic buffers, not only nfsmw-nx's).

## Building

### Windows, MSYS2 (`build-unified.sh`)

What the build needs from MSYS2 is packages: NVK compiles two OpenCL kernels (`src/nouveau/vulkan/cl/*.cl`) with
`mesa_clc`, a host tool built from this tree with LLVM, Clang, SPIRV-LLVM-Translator and SPIRV-Tools, and the
cross build uses meson, python and bindgen from CLANGARM64. Any MSYS2 that can install those works. devkitPro's
own MSYS2 (`C:/devkitPro/msys2`) is a normal MSYS2 that ships with only the MSYS and devkitPro repositories;
`bash build-nfsmw.sh --enable-msys2-repos` adds MSYS2's standard ones to it (keeping a backup of
`/etc/pacman.conf`), so no second MSYS2 is needed. A separate MSYS2 at `C:/msys64` works as well.
devkitPro itself can stay where it is (`C:/devkitPro`, or `C:/msys64/opt/devkitpro`): the script finds it
(or takes `DEVKITPRO`) and writes copies of the repository's MSYS2 build files, which hardcode
`C:/msys64/opt/devkitpro`, with the real paths (`build/nfsmw-msys2/`). Scripts are not on `PATH`, so start them
with `bash`:

```sh
bash build-nfsmw.sh --check              # what is missing, with the command that installs it
bash build-nfsmw.sh --install-packages   # install the missing MSYS2 and devkitPro packages (pacman)
bash build-nfsmw.sh --setup-rust         # install the Rust toolchain the build expects, into build/deps
bash build-nfsmw.sh                      # check, then build
```

`build-nfsmw.sh` is a shortcut for `nfsmw/build-msys2.sh`; the options can be combined
(`bash build-nfsmw.sh --install-packages --setup-rust`).

What the repository's MSYS2 files expect, and what the script checks:

| Where | What | Packages |
|---|---|---|
| CLANGARM64 on an ARM64 PC, CLANG64 on an x64 PC (ARM64 programs do not run on x64 Windows) | meson 1.7+, ninja, python with mako, pyyaml, packaging, setuptools, clang, bindgen 0.71.1+ (not 0.72.0), cbindgen | `mingw-w64-clang-aarch64-…` or `mingw-w64-clang-x86_64-{meson,ninja,python,python-mako,python-yaml,python-packaging,python-setuptools,clang,rust-bindgen,cbindgen}` |
| CLANG64 | host tools (`mesa_clc`): clang, llvm, SPIRV-LLVM-Translator, SPIRV-Tools, pkgconf, cmake | `mingw-w64-clang-x86_64-{clang,llvm,spirv-llvm-translator,spirv-tools,pkgconf,cmake}` |
| UCRT64 | gcc, which links the Rust proc macros | `mingw-w64-ucrt-x86_64-gcc` |
| MSYS | git, flex, bison, pkg-config | `git flex bison pkgconf` |
| devkitPro (`C:/devkitPro` or `C:/msys64/opt/devkitpro`; installed with devkitPro's pacman) | devkitA64, libnx, zlib, zstd, expat | `switch-dev libnx switch-zlib switch-libzstd switch-libexpat` |
| Rust | x86_64-pc-windows-gnu host toolchain with the aarch64-unknown-linux-gnu std (the same rustc builds the Switch code and the proc macros it loads, which the x64 GCC links) | `--setup-rust` installs it into `build/deps`, where `build-unified.sh` looks |

The repository's own MSYS2 files were written on a Windows-on-ARM PC (native tools from CLANGARM64, x64 host tools
under emulation). The script detects the PC: on an x64 PC it takes the native tools from CLANG64 and points its copies
of the build files at them (`NFSMW_TOOLS_ENV=clang64|clangarm64` overrides the detection).
If `cbindgen` is somewhere other than `C:/msys64/clangarm64/bin`, the script writes a copy of
`switch_native_tools_msys2.txt` with the right path. If only an x64 `bindgen` from cargo is available, it points
`LIBCLANG_PATH` at CLANG64's libclang.

Before building, the script gives the tree a git repository if it has none (`build-unified.sh` takes the build id
and `SOURCE_DATE_EPOCH` from git) and sets `ALLOW_DIRTY=1`. The driver ends up in
`mesa-unified-install/opt/devkitpro/portlibs/switch/lib/libvulkan.a`, and an SDK zip in `dist/`.

libelf is optional for this build: without it the CUBIN parser (`nv_cubin.c`, only used by
`VK_NVX_binary_import`) is left out.

### Docker (`build-switch.sh`), Windows or Linux

The repository's Docker build runs everything inside a Linux container: the image (`Docker.rust`) brings
devkitA64, LLVM, SPIRV-Tools, Rust and bindgen, so nothing has to be installed on the host besides Docker
(Docker Desktop on Windows, running). Its output is `builddir-switch/src/nouveau/vulkan/libvulkan.a`, the layout
UnleashedRecomp's build script was written for.

```sh
bash build-nfsmw.sh --docker     # or: bash build-switch.sh
```

It works from any shell with `docker` on its path: devkitPro's MSYS2, Git Bash, WSL, Linux. From MSYS2 and Git
Bash, `build-switch.sh` now passes Docker the Windows path of this folder and stops MSYS from rewriting the
arguments of `docker.exe` (which otherwise mangles the `-v` mount and the `/project/...` paths in the container
commands). The first run builds the image, which takes a while and a few GB.

## Using it with UnleashedRecomp

`tools/build-switch.sh` accepts any of the layouts: the Docker tree, a tree built with `build-unified.sh`, an
extracted SDK or its `portlibs/switch` folder.

```sh
NVK_ROOT=/c/path/to/mesa-switch-main tools/build-switch.sh
```

- Delete UnleashedRecomp's `build/` folder before rebuilding: Ninja does not see that a driver archive outside the
  build tree changed and would keep linking the old one.
- The pipeline cache (`cache/pipelines.bin` in the user folder) is keyed to the driver and is rebuilt on its own;
  deleting it does no harm.
- The UnleashedRecomp tree links `expat` and `drm_nouveau` only when devkitPro has them, puts the driver in a link
  group, and carries weak libelf stubs (the driver's CUBIN parser references libelf, which devkitPro does not
  ship; it never runs on Horizon).
- ZCULL needs depth images without `TRANSFER_DST`; UnleashedRecomp's plume patch already creates them that way.
- `[Switch] SwitchMesaEnvironment = "NAME=value;NAME=value"` in UnleashedRecomp's config sets driver variables
  before the device is created, e.g. `"NVK_SWITCH_DIBUJO=0"` to compare with the draw-path changes off.

## Checking that it works

UnleashedRecomp writes stderr to `sdmc:/switch/UnleashedRecomp/stderr.log`. Look for:

- `nvkmd-switch: ZCULL activado (...)` and `nvkmd-switch: contexto de ZCULL atado (...)`: the kernel reported the
  ZCULL geometry and the channel has its ZCULL context.
- `[nvk] ZCULL: plano de N bytes para profundidad WxH`: a depth image got a ZCULL plane (first 8 sizes).
  `[nvk] ZCULL: SIN plano ...` says why one did not (usually `TRANSFER_DST`).
- `NVK fast paths: requested (set 4 contract found, draw contract found)`: UnleashedRecomp found the driver's shared
  structures. A line about a self-check difference means a driver path turned itself off for the session.

Run-time variables read by the driver: `NVK_SWITCH_DIBUJO`, `NVK_SWITCH_DYN_UBO_DELTA`, `NVK_COPY_ENGINE`,
`NVK_SWITCH_NO_UBO_CBUF`, `NVK_SUBTILING_KNOB`, `NVK_SHADER_STATS` (shader statistics; check that local memory
stays at 0 with the branch flattening), `NVK_SWITCH_CMD_MEM_CPU_UNCACHED`, `NVK_SWITCH_CPU_WRITE_MEM_UNCACHED`,
`NVK_SWITCH_MEM_STREAM_CPU_UNCACHED`.

## Updating to a newer mesa-switch later

In a git checkout of the newer mesa-switch: `git apply --3way nfsmw/mesa-switch-nfsmw-26.2.2.patch` (with the old
tree's blobs available, for example after fetching this commit), or merge a branch holding this tree. Resolve
conflicts the same way as above, and raise the NAK revision in `nvk_shader.c` whenever a NAK change is added.
