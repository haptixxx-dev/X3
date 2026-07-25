# Build and Tooling Dependencies

Everything the migration needs, and when. Verified against this machine
(Arch, GCC 16.1.1, Vulkan 1.4.350, RTX 2060 + RTX 5070) on 2026-07-25.

Vendored libraries under `X3/libs/` and `X3-Editor/libs/` are not listed —
they come with the checkout. This is only what must exist on the system.

## Install now

Two are already in and were blocking everything:

```
sudo pacman -S vulkan-headers vulkan-validation-layers   # DONE
```

`vulkan-headers` blocks *all* builds, not just the Vulkan preset, because
imgui pulled in `find_package(Vulkan REQUIRED)` unconditionally. Validation
layers are the only way to verify Phase 1's synchronization work.

The rest are worth having before Phase 1 starts in earnest:

```
sudo pacman -S renderdoc vulkan-extra-layers vulkan-extra-tools ccache mold
```

| Package | Why |
|---|---|
| `renderdoc` | Frame capture and inspection. The practical way to debug a descriptor or barrier problem once validation says something is wrong but not where. |
| `vulkan-extra-layers` | Adds the API dump and other Khronos layers beyond core validation. |
| `vulkan-extra-tools` | Provides `vkconfig`, the sane way to toggle synchronization validation and best-practices without wrangling environment variables. |
| `ccache` | Rebuild speed. A clean build of this tree is several minutes, dominated by assimp and Jolt, and Phase 1 involves many rebuilds. |
| `mold` | Link speed. `X3Editor` is a 38 MB debug binary and gets relinked constantly. |

## Already present

`vulkan-tools`, `shaderc` (`glslc` 2026.2), `spirv-tools` (`spirv-dis`,
`spirv-val`), `glslang`, `cmake` 4.4.0, `ninja`, `gcc` 16.1.1, `clang` 22.1.8,
`lld`, `gdb`, `minizip`, `glfw` 3.4.0, `glew` 2.3.1.

## Later phases

None of these block current work; listed so they are not a surprise.

**Phase 3 — Slang.** Not in the Arch repositories. Note the installed
`extra/slang 2.3.3` package is **S-Lang, the interpreted language** — an
unrelated project with a colliding name. The shader compiler is
`shader-slang`, from GitHub releases (prebuilt Linux x86_64 tarballs,
currently ~v2026.14, roughly weekly cadence) or the vcpkg port. Decide
whether to vendor a pinned release in `X3/libs/` for reproducibility or
depend on a system install; vendoring is probably right given the release
cadence.

**Phase 4 — job system.** enkiTS or Taskflow. Header-light, add as a
submodule rather than a system package.

**Phase 9 — asset cook.** `meshoptimizer` for vertex cache and overdraw
optimization, and a BC7 encoder. Neither is in the Arch repos; both are
small and vendor cleanly. `bc7enc` is the least-friction BC7 option.

**Phase 10 — GI.** `xatlas` for lightmap UV unwrapping. Not packaged;
vendor it.

**Phase 11 — post stack.** The XeSS SDK, downloaded from Intel. This is
the vendor-neutral upscaler choice; its cross-vendor DP4a path runs on the
NVIDIA hardware here.

**Phase 12 — OpenPBR import.** Either the MaterialX C++ library as a
submodule (note the previous gitlink was broken and has been removed), or a
targeted `.mtlx` parser. Only document parsing is needed, not ShaderGen, so
the smaller option is likely sufficient.

**Optional, NVIDIA-specific.** Nsight Graphics is not in the Arch repos and
comes from NVIDIA directly. Its 2025.4 release added beta Slang source-level
debugging, which becomes relevant at Phase 3.

## Verification

`scripts/verify.sh` builds every preset from scratch and smoke-tests the
binaries. It deliberately checks produced artefacts and greps logs rather
than trusting exit codes — a piped build already reported a false success
once during this migration.

**Its smoke test only covers initialization.** The editor boots to a project
launcher and `RenderLayer` renders nothing until a project is open, so a bare
launch exercises no dispatch, no descriptor updates, and no swapchain blit.
Render-path verification requires opening a project with a scene.
