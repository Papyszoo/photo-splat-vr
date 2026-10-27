// splat_convert: model checkpoint -> GGUF for the ggml backends.
//
//   splat_convert [--backend sharp|da2] [--fp32] <in> <out.gguf>
//
// sharp (C1/C3): Apple SHARP .pt (torch zip/pickle), pinned SHA-256.
// da2   (C4)   : Depth Anything V2 Small .safetensors, pinned SHA-256.
//
// Both profiles share the same writer and policy: stream one tensor at a time,
// fp16 for 2-D and higher tensors, fp32 for 1-D, metadata recording the source
// hash and the disclosed changes. --fp32 keeps everything fp32 (parity runs).

#include <cinttypes>
#include <cstdio>
#include <string>

#include <sys/resource.h>

#include "common/bytes.h"
#include "model/checkpoint.h"
#include "model/converter.h"
#include "model/da2_profile.h"

namespace {

using splat::human_bytes;

uint64_t peak_rss_bytes() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<uint64_t>(usage.ru_maxrss);  // bytes on macOS
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024;  // KiB elsewhere
#endif
}

void usage() {
    std::fprintf(stderr, "usage: splat_convert [--backend sharp|da2] [--fp32] <in> <out.gguf>\n");
    std::fprintf(stderr, "  sharp  Apple SHARP checkpoint (.pt), pinned SHA-256 (default)\n");
    std::fprintf(stderr, "  da2    Depth Anything V2 Small (.safetensors), pinned SHA-256\n");
    std::fprintf(stderr, "  --fp32 keep every tensor float32 (parity runs; default is fp16 weights)\n");
    std::fprintf(stderr, "  The pinned SHA-256 is verified before conversion; the output is\n");
    std::fprintf(stderr, "  labelled as a converted derivative in its metadata.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    splat::ConvertOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            usage();
            return 0;
        }
        if (argument == "--backend") {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            options.backend = argv[++i];
            if (options.backend != "sharp" && options.backend != "da2") {
                std::fprintf(stderr, "splat_convert: unknown backend '%s'\n", options.backend.c_str());
                return 2;
            }
        } else if (argument.rfind("--backend=", 0) == 0) {
            options.backend = argument.substr(10);
            if (options.backend != "sharp" && options.backend != "da2") {
                std::fprintf(stderr, "splat_convert: unknown backend '%s'\n", options.backend.c_str());
                return 2;
            }
        } else if (argument == "--fp32") {
            options.keep_fp32 = true;
        } else if (input.empty()) {
            input = argument;
        } else if (output.empty()) {
            output = argument;
        } else {
            std::fprintf(stderr, "splat_convert: unexpected argument '%s'\n", argument.c_str());
            usage();
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        usage();
        return 2;
    }

    const bool da2 = options.backend == "da2";
    std::printf("input    %s\n", input.c_str());
    std::printf("profile  %s%s\n", options.backend.c_str(), options.keep_fp32 ? " (fp32 weights)" : "");
    std::fflush(stdout);  // keep the header above the progress lines on stderr

    splat::Progress progress([](float fraction, const std::string& stage) {
        static int last_decile = -1;
        const int decile = static_cast<int>(fraction * 10.0f);
        if (decile > last_decile) {
            last_decile = decile;
            std::fprintf(stderr, "  %3d%% %s\n", decile * 10, stage.c_str());
        }
    });

    const splat::Result<splat::ConvertStats> result =
        da2 ? splat::convert_depth_anything_v2(input, output, options, progress)
            : splat::convert_checkpoint(input, output, options, progress);
    if (!result.ok()) {
        std::fprintf(stderr, "splat_convert: %s\n", result.message().c_str());
        return 1;
    }
    const splat::ConvertStats& stats = result.value();
    const bool pinned = da2 ? stats.source_sha256 == splat::kDepthAnythingV2SmallSha256
                            : stats.source_sha256 == splat::kSharpCheckpointSha256;

    std::printf("sha256   %s%s\n", stats.source_sha256.c_str(), pinned ? "  (pinned, verified)" : "");
    std::printf("zip64    %s\n", stats.zip64 ? "yes" : "no");
    std::printf("source   %" PRIu64 " bytes (%s)\n", stats.source_bytes,
                human_bytes(stats.source_bytes).c_str());
    std::printf("gguf     %s\n", output.c_str());
    std::printf("bytes    %" PRIu64 " (%s)\n", stats.gguf_bytes, human_bytes(stats.gguf_bytes).c_str());
    std::printf("tensors  %" PRIu64 " written, %" PRIu64 " dropped\n", stats.tensor_count,
                stats.dropped_count);
    std::printf("seconds  %.3f\n", stats.seconds);
    std::printf("peak rss %s (%" PRIu64 " bytes)\n", human_bytes(peak_rss_bytes()).c_str(),
                peak_rss_bytes());
    return 0;
}
