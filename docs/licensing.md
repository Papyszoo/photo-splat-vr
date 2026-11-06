# Licensing

This project is GPL-3.0-or-later (see `LICENSE`). Third-party components and
their licences are listed in `THIRD_PARTY_NOTICES.md`; the licence texts
themselves are in `third_party/LICENSES/`.

## Static linking of LGPL libraries

The console title and the desktop build link the LGPL libraries statically:
libsmb2 and libnfs (LGPL-2.1-or-later), LibRaw (LGPL-2.1, the CDDL-1.0
alternative is not taken), libheif and libde265 (LGPL-3.0-or-later). Static
linking into this GPL-3.0-or-later work is allowed because the complete
corresponding source is available in this public tree and the LGPL notices are
kept.

The libraries are unmodified. Their sources are the git submodules under
`third_party/` (each pinned to the tag named in `THIRD_PARTY_NOTICES.md`), and
the build recipes are in the repository: the CMake lines in `CMakeLists.txt`
and `platform/ps5/ggml/CMakeLists.txt`, `third_party/libraw_sources.cmake`,
`third_party/zstd_sources.cmake`, and `third_party/build_codecs.sh` for
libheif/libde265/libaom. Anyone can therefore relink the application against a
modified version of any of them: edit the submodule, rebuild with the same
commands, and link as the gate does.

No prebuilt `.a` is shipped. Every archive is produced from these submodules by
the repository's own build scripts.
