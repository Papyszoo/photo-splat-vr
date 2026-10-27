# platform/ps5/ggml — cross build of ggml + the project core for a payload

Builds the reconstruction stack for the PS5 payload target (x86-64 Sie/FreeBSD,
`prospero-clang` from the payload SDK): **ggml 0.27.0 with the CPU and Vulkan
backends**, and the project's core sources, as static libraries that
`platform/ps5/bench/` links into payloads with the same RADV archive `vk_hello`
uses (no Vulkan loader).

```sh
platform/ps5/ggml/build.sh                 # build-ps5-ggml/
platform/ps5/ggml/build.sh --clean         # from scratch: 63 s wall, 8 jobs
make -C platform/ps5/bench                 # op_audit/da2_bench/sharp_bench
platform/ps5/bench/verify-elfs.sh          # static checks, no console
```

Needs, all outside the repo (AGENTS.md):

| Input | Where | Why |
| --- | --- | --- |
| PS5 payload SDK | `$SPLAT_DATA_DIR/ref/radv-ps5/sdk` (or `$PS5_PAYLOAD_SDK`) | `prospero-clang`, sysroot, libc++ |
| RADV archive | `$SPLAT_DATA_DIR/ref/radv-ps5/lib/libvulkan_radeon.ps5.a` | final link, whole archive |
| Host clang + `LLVM_CONFIG` | Homebrew `llvm@21` | the SDK's clang wrapper finds clang through it |
| `glslc` (shaderc) | Homebrew | shaders are compiled **on the host** |
| SPIRV-Headers | Homebrew | Vulkan-Hpp include set for ggml-vulkan |

## What it builds

| Target | Size | Contents |
| --- | ---: | --- |
| `libggml-base.a` | 5.2 MB | ggml core, GGUF reader, backend registry |
| `libggml-cpu.a` | 4.9 MB | CPU backend (AVX2/FMA/F16C/BMI2, Zen 2 baseline; no AVX-512) |
| `libggml.a` | 0.46 MB | dynamic-backend registry objects |
| `libggml-vulkan.a` | 54.8 MB | Vulkan backend + **155 shaders** generated here by ggml's `vulkan-shaders-gen` and embedded as `uint32_t[]` |
| `libsplat_core_ps5.a` | 8.2 MB | `core/common`, `core/scene`, `core/recon/{ggml,fallback,sharp}`, `core/jobs/image_io.cpp` |
| `libsplat_miniz.a` | 0.39 MB | the vendored miniz the core links |
| `libsplat_ps5_glue.a` | 0.75 MB | the loader-less Vulkan shims, `bench.env` loader, submit counter, watchdog, `vk_reset.cpp` |

`core/jobs/image_io.cpp` is the one file from `core/jobs`: both benches have to
decode a JPEG, and re-implementing the stb wrapper would duplicate project code.
Nothing from `core/render`, `core/ingest`, `core/model`'s converter (zip, zlib,
HTTP) or the desktop viewer is in a reconstruction payload.

## How the Vulkan backend is configured without a loader

ggml's own CMake asks for `Vulkan` and `SPIRV-Headers`. Both are satisfied
without a Vulkan SDK and without a loader:

* `cmake/vulkan/VulkanConfig.cmake` is a stand-in package: it sets
  `Vulkan_FOUND`, hands over the **headers** and defines `Vulkan::Vulkan` as an
  interface target with **no library**. `find_package(Vulkan COMPONENTS glslc)`
  succeeds, `target_link_libraries(ggml-vulkan PRIVATE Vulkan::Vulkan)` links
  nothing, and the entry points come from RADV at the final ELF link.
* The headers are the **host's** Vulkan-Headers (1.4.363), not the RADV build's
  1.4.354 C header: ggml-vulkan needs Vulkan-Hpp, and `vulkan.hpp` static-asserts
  that `VK_HEADER_VERSION` matches the `vulkan_core.h` it is compiled against.
  The pair must come from one source. The 1.4 core ABI and every KHR structure
  ggml uses are unchanged between the two, and no library is taken from them.
* `GGML_VULKAN_SHADERS_GEN_TOOLCHAIN` points at `host-toolchain.cmake`, so
  ggml's shader generator is built for this Mac and runs here during the cross
  build. The console receives only the embedded SPIR-V arrays.

### The four Vulkan entry points a payload must provide

RADV's archive exports `vk_icdGetInstanceProcAddr` and nothing else by name.
ggml-vulkan references four symbols directly; `ps5_vulkan.cpp` defines each as
a one-line forward through the ICD entry, with the instance captured from the
dispatcher's first call — the same lookup a loader does, no layers:

| Symbol | Referenced by |
| --- | --- |
| `vkGetInstanceProcAddr` | vulkan-hpp `DispatchLoaderDynamic::init` |
| `vkGetPhysicalDeviceFeatures2` | `ggml_vk_get_device` feature chain |
| `vkGetDeviceProcAddr` | `VK_EXT_device_fault` hook |
| `vkCmdCopyBuffer` | `ggml_vk_buffer_copy_async` |

Plus three libc gaps the PS5 libc does not export:
`fork`/`execlp` (ggml's crash handler would fork gdb; it now takes the "no
debugger" path) and `pathconf` (libc++ `std::filesystem::current_path`).

**No ggml source is patched.** `patches/` has no entry for this task; the shims
live in the payload, not in the submodule.

## Cooperative matrices on and off in one `op_audit` launch

Cooperative-matrix support is decided once, when ggml-vulkan builds the
`vk_device_struct` it caches in `vk_instance.devices[idx]`, and freeing the
backend does not drop that cache. `op_audit` needs the same cases at both
settings in one process, so `vk_reset.cpp` (part of this glue library) includes
ggml-vulkan's own `ggml-vulkan-types.h`, breaks the device's two self-references
(`sync_staging` and the `buffer_type.context` that holds the device) and drops
the cache entry; `ggml_backend_dev_init` then builds a fresh device with the
current `GGML_VK_DISABLE_COOPMAT`. It runs only between completed passes, when
every buffer is freed and the GPU is idle. This is the one place a payload
reaches into ggml-vulkan internals; it is not a core/ dependency and not a
submodule patch, and a future upstream device-reset call would replace it.

## Two build-time facts worth knowing

* `GGML_VULKAN_SHADERS_GEN_TOOLCHAIN` is required: without it the nested
  configure inherits the PS5 toolchain and tries to run a FreeBSD binary here.
* The SDK's `clang` wrapper resolves the real compiler through `LLVM_CONFIG`.
  Without it the build fails with `/llvm-clang++: No such file or directory`,
  which looks like a missing SDK file but is just the unset variable.
  `build.sh` sets it when Homebrew's `llvm@21` is present.

## See also

The payloads call `bench::apply_radv_defaults()` before their first GPU call,
which sets the two driver settings `vk_hello` uses
(`MESA_SHADER_CACHE_DISABLE=true`, `radv_enable_unified_heap_on_apu=true`)
without overwriting, so `bench.env` can still change them.

* `docs/research/spike-a.md` — the Spike A record: payloads, sizes, host smoke
  results, the console runbook and the failure table.
* `docs/research/spike-b.md` — RADV build and `vk_hello`, which this build reuses.
* `core/recon/ggml/README.md` — what the libraries contain and how the graph is
  split into bounded submits.
