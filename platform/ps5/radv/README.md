# platform/ps5/radv — build glue for the PS5 RADV archive

`build-in-docker.sh` builds the release RADV archive
(`libvulkan_radeon.ps5.a`: RADV + ACO + NIR + Mesa's Vulkan runtime) for the
console and installs it under `$SPLAT_DATA_DIR/ref/radv-ps5`, outside the repo:

| Path | What |
| --- | --- |
| `lib/libvulkan_radeon.ps5.a` | the archive titles and payloads link whole |
| `lib/libclang_rt.builtins-x86_64.a` | Clang builtins, for `__emutls_get_address` |
| `lib/libSceAgc.so`, `lib/libSceAgcDriver.so` | AGC import stubs (SONAME `libSceAgc.sprx`), built from PS5_Vulkan's canary sources |
| `include/vulkan`, `include/vk_video` | the headers the archive was built with |
| `sdk/` | the pinned PS5_PayloadSDK fork, with `libps5platform.a` |
| `BUILD.txt` | revisions, container, build seconds, SHA-256 |

```sh
platform/ps5/radv/build-in-docker.sh
export PS5_PAYLOAD_SDK="$SPLAT_DATA_DIR/ref/radv-ps5/sdk"
export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
make -C platform/ps5/vk_hello vk_hello.elf
```

The container is `debian:trixie-slim` on `linux/amd64`, capped at 2 CPUs and
5 GiB, mirroring PS5LM's CI (`docs/BUILDING.md`, `.github/workflows/native-build.yml`).
The pinned revisions are PS5_Vulkan `f410e67`'s Mesa:
`1ba9874f478657ccf6da72ce55f0260164cc0bd0`, with PS5_PayloadSDK
`3e31e0b1b68ada28beca68adc480cf0c3d17b9df` (the fork's current revision;
PS5_Vulkan's bootstrapper pins the older `b83202b`, which lacks the
`ps5_readlink`/`ps5_mkstemp`/`ps5_localeconv` libc shims RADV's link recipe
expects and a payload has no title `libc.prx` to supply). The archive is built
against the same SDK it is linked with. PS5LM's app-specific Mesa patches are
not applied; the route and timing are in `docs/research/spike-b.md`.

The script copies the clones from `$SPLAT_DATA_DIR/ref` into a throwaway
container with `docker cp` and builds in `/work/src` there; the host keeps only
the log, in `$SPLAT_DATA_DIR/runs/B1/radv-docker`. A re-run starts from a fresh
copy (the image layers are cached; the Meson tree is not).
