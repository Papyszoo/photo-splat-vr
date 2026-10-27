# Shared RADV link recipe for loader-less PS5 payloads.
#
# platform/ps5/vk_hello's Makefile proved the recipe (docs/research/spike-b.md
# §3.1, PS5_Vulkan's tools/radv-link.sh adapted to the payload SDK's crt and
# libc): RADV whole (its dispatch tables name entry points through weak
# references), libc++/libc++abi for ACO, libunwind, Clang's builtins for
# __emutls_get_address, the SDK fork's platform layer for libc functions no
# system module exports, and the AGC/VideoOut imports.
#
# The bench payloads need exactly the same inputs plus their own libraries and
# an extra --wrap for the submit counter, so the four lists and the flags live
# here and both Makefiles include this file. Change the recipe once.
#
# Expects the including Makefile to have defined, before the include:
#   RADV_PS5      the RADV build directory (lib/, sdk/, include/)
#   RADV_SDK      the payload SDK RADV was built against
#   RADV_LIB      libvulkan_radeon.ps5.a
#   RADV_BUILTINS libclang_rt.builtins-x86_64.a
#
# Defines:
#   RADV_LINK_BASE    -nodefaultlibs, build-id, RADV whole-archive
#   RADV_SDK_LIBS     the runtime archives every payload puts in --start-group
#   RADV_WRAPS        the --wrap list (threads and the platform heap)
#   RADV_DEFSYMS      NAME -> ps5_NAME for libc functions the platform layer
#                     supplies (the list is radv-link.sh's)
#   RADV_SYSTEM_LIBS  the AGC/VideoOut/libc import stubs
#
# SPDX-License-Identifier: GPL-3.0-or-later

RADV_COMMA := ,

#   --wrap=pthread_*   threads get their stacks from direct memory
#   --wrap=malloc...   allocations go to the platform heap in direct memory
RADV_WRAPS := $(addprefix -Wl$(RADV_COMMA)--wrap$(RADV_COMMA),pthread_create pthread_join \
                pthread_detach fclose fflush malloc calloc realloc free posix_memalign \
                aligned_alloc memalign malloc_usable_size reallocf reallocarray getline getdelim)

RADV_DEFSYM_NAMES := qsort_r mkstemps openlog popen pclose \
                open_memstream __xuname __assert __memset_chk regcomp regexec regfree regerror \
                localtime_r newlocale freelocale strtod_l strtof_l dladdr utimensat localeconv_l \
                strtoll_l strtoull_l strtold_l snprintf_l sscanf_l asprintf_l strcoll_l strxfrm_l \
                strftime_l wcscoll_l wcsxfrm_l btowc_l wctob_l iswctype_l mbrlen_l mbrtowc_l \
                mbsrtowcs_l mbsnrtowcs_l wcrtomb_l wcsnrtombs_l mbtowc_l ___mb_cur_max_l \
                ___runetype_l ___tolower_l ___toupper_l __runes_for_locale catopen catgets \
                catclose backtrace backtrace_symbols_fd __cxa_thread_atexit_impl arc4random \
                arc4random_buf arc4random_uniform gmtime_r statvfs fstatvfs futimens \
                clock_nanosleep getaddrinfo freeaddrinfo if_nameindex if_freenameindex opendir \
                fdopendir readdir rewinddir dirfd closedir nl_langinfo nl_langinfo_l getpwuid_r \
                posix_fallocate access openat unlinkat fchmodat fstatat mkdirat renameat memfd_create \
                localeconv strcasestr mkstemp readlink link symlink
RADV_DEFSYMS := $(foreach n,$(RADV_DEFSYM_NAMES),-Wl$(RADV_COMMA)--defsym$(RADV_COMMA)$(n)=ps5_$(n))

RADV_SDK_LIBS := $(RADV_SDK)/target/lib/libc++.a \
                 $(RADV_SDK)/target/lib/libc++abi.a \
                 $(RADV_SDK)/target/lib/libunwind.a \
                 $(RADV_SDK)/target/lib/libps5platform.a \
                 $(RADV_BUILTINS)

RADV_SYSTEM_LIBS := -L$(RADV_PS5)/lib -lSceAgc -lSceAgcDriver -lSceLibcInternal \
                    -lkernel_web -lSceVideoOut

RADV_LINK_BASE := -nodefaultlibs -Wl,--build-id=sha1 \
                  -Wl,--whole-archive $(RADV_LIB) -Wl,--no-whole-archive
