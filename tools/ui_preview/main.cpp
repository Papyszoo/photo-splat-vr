// ui_preview: render panel screens to PNGs without the console.
//
//   ui_preview <out.png> [pattern]
//   ui_preview --all <dir>        every panel screen as <dir>/<name>.png
//
// Each pattern names its canvas and theme scale (patterns.h), so the VR
// previews are drawn at the headset's 896x504 with the 0.7 theme. The pattern
// registry lives in patterns.cpp, so a new screen is one line there and nothing
// here changes.

#include <cstdio>
#include <string>

#include "patterns.h"
#include "ui/canvas.h"

namespace {

void usage() {
    std::fprintf(stderr,
                 "usage: ui_preview <out.png> [pattern]\n       ui_preview --all <dir>\n\n"
                 "  patterns:\n");
    for (const auto& entry : splat::ui_preview::registry()) {
        std::fprintf(stderr, "    %-12s %ux%u%s\n", entry.first.c_str(), entry.second.width,
                     entry.second.height,
                     entry.first == "pattern" ? " (default) built-in test pattern" : "");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        usage();
        return 2;
    }

    if (argc == 3 && std::string(argv[1]) == "--all") {
        int failed = 0;
        for (const auto& entry : splat::ui_preview::registry()) {
            if (entry.first == "pattern") continue;  // a diagnostic, not a panel screen
            splat::UiCanvas canvas(entry.second.width, entry.second.height);
            entry.second.draw(canvas);
            const std::string path = std::string(argv[2]) + "/" + entry.first + ".png";
            const splat::Status written = canvas.write_png(path);
            if (!written.ok()) {
                std::fprintf(stderr, "ui_preview: %s\n", written.message().c_str());
                ++failed;
                continue;
            }
            std::printf("ui_preview: wrote %s\n", path.c_str());
        }
        return failed == 0 ? 0 : 1;
    }

    const std::string name = argc == 3 ? argv[2] : "pattern";
    const splat::ui_preview::Registry& patterns = splat::ui_preview::registry();
    const auto found = patterns.find(name);
    if (found == patterns.end()) {
        std::fprintf(stderr, "ui_preview: unknown pattern '%s'\n", name.c_str());
        usage();
        return 2;
    }

    splat::UiCanvas canvas(found->second.width, found->second.height);
    found->second.draw(canvas);

    const splat::Status written = canvas.write_png(argv[1]);
    if (!written.ok()) {
        std::fprintf(stderr, "ui_preview: %s\n", written.message().c_str());
        return 1;
    }
    std::printf("ui_preview: wrote %s (%s, %ux%u)\n", argv[1], name.c_str(), canvas.width(),
                canvas.height());
    return 0;
}
