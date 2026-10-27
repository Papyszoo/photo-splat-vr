// splat_model: manual driver for the C1 model manager.
//
//   splat_model [--state-dir DIR] status [backend]
//   splat_model [--state-dir DIR] accept <backend>
//   splat_model [--state-dir DIR] fetch <backend>
//   splat_model [--state-dir DIR] import <file>
//
// `accept` fetches the licence text from its official URL, prints it, and only
// then records the acceptance. `fetch` downloads with resume, verifies the
// pinned SHA-256 and converts to GGUF; it also picks up a checkpoint placed in
// <state>/inbox/. `import` does the same for a file the user already has.
//
// The state directory comes from --state-dir, then SPLAT_STATE_DIR, then
// ./splat-state. The console app passes its own /data/<app> directory.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "common/bytes.h"
#include "model/model_manager.h"
#include "model/sha256.h"
#include "platform/desktop/curl_http_fetcher.h"

namespace {

using splat::human_bytes;

void usage() {
    std::fprintf(stderr,
                 "usage: splat_model [--state-dir DIR] status [backend]\n"
                 "       splat_model [--state-dir DIR] accept <backend>\n"
                 "       splat_model [--state-dir DIR] fetch <backend>\n"
                 "       splat_model [--state-dir DIR] import <file>\n"
                 "\n"
                 "  status  print the persisted state of every backend, or one\n"
                 "  accept  fetch and print the model licence, then record acceptance\n"
                 "  fetch   download (resuming), verify and convert a backend's checkpoint\n"
                 "  import  verify and convert a checkpoint the user already has\n"
                 "  DIR     defaults to $SPLAT_STATE_DIR or ./splat-state\n");
}

void print_record(const splat::ModelRecord& record) {
    std::printf("%-5s %s\n", record.backend.c_str(), splat::model_state_name(record.state));
    if (!record.message.empty()) std::printf("      %s\n", record.message.c_str());
    if (!record.source_path.empty()) {
        std::printf("      source   %s\n", record.source_path.string().c_str());
    }
    if (!record.gguf_path.empty()) {
        std::printf("      gguf     %s\n", record.gguf_path.string().c_str());
    }
    if (!record.source_sha256.empty()) {
        std::printf("      sha256   %s\n", record.source_sha256.c_str());
    }
    if (record.source_bytes > 0) {
        std::printf("      bytes    %llu (%s)\n",
                    static_cast<unsigned long long>(record.source_bytes),
                    human_bytes(record.source_bytes).c_str());
    }
    if (record.state == splat::ModelState::LicenseRequired) {
        std::printf("      run 'splat_model accept %s' to read and accept its licence\n",
                    record.backend.c_str());
    }
}

splat::ProgressCallback progress_printer() {
    return [](float fraction, const std::string& stage) {
        static int last_decile = -1;
        const int decile = static_cast<int>(fraction * 10.0f);
        if (decile > last_decile) {
            last_decile = decile;
            std::fprintf(stderr, "  %3d%% %s\n", decile * 10, stage.c_str());
        }
    };
}

}  // namespace

int main(int argc, char** argv) {
    std::string state_dir;
    std::string command;
    std::string argument;

    for (int index = 1; index < argc; ++index) {
        const std::string value = argv[index];
        if (value == "-h" || value == "--help") {
            usage();
            return 0;
        }
        if (value == "--state-dir") {
            if (index + 1 >= argc) {
                usage();
                return 2;
            }
            state_dir = argv[++index];
        } else if (value.rfind("--state-dir=", 0) == 0) {
            state_dir = value.substr(12);
        } else if (command.empty()) {
            command = value;
        } else if (argument.empty()) {
            argument = value;
        } else {
            std::fprintf(stderr, "splat_model: unexpected argument '%s'\n", value.c_str());
            usage();
            return 2;
        }
    }

    if (state_dir.empty()) {
        const char* from_env = std::getenv("SPLAT_STATE_DIR");
        state_dir = from_env != nullptr && from_env[0] != '\0' ? from_env : "splat-state";
    }

    splat::CurlHttpFetcher fetcher;
    splat::ModelManagerOptions options;
    options.state_dir = state_dir;
    options.fetcher = &fetcher;
    splat::ModelManager manager(std::move(options));

    if (command == "status") {
        if (argument.empty()) {
            for (const splat::ModelRecord& record : manager.status_all()) print_record(record);
            return 0;
        }
        const splat::Result<splat::ModelRecord> record = manager.status(argument);
        if (!record.ok()) {
            std::fprintf(stderr, "splat_model: %s\n", record.message().c_str());
            return 1;
        }
        print_record(record.value());
        return 0;
    }

    if (command == "accept") {
        if (argument.empty()) {
            usage();
            return 2;
        }
        const splat::Result<std::string> text = manager.fetch_license_text(argument);
        if (!text.ok()) {
            std::fprintf(stderr, "splat_model: %s\n", text.message().c_str());
            return 1;
        }
        const splat::ModelBackendSpec* spec = manager.backend(argument);
        std::printf("--- %s, %s ---\n", spec->license_name.c_str(), spec->license_url.c_str());
        std::printf("%s\n", text.value().c_str());
        if (!text.value().empty() && text.value().back() != '\n') std::printf("\n");
        std::printf("--- end of licence ---\n");

        const splat::Status recorded = manager.accept_license(argument, text.value());
        if (!recorded.ok()) {
            std::fprintf(stderr, "splat_model: %s\n", recorded.message().c_str());
            return 1;
        }
        std::printf("accepted %s: licence sha256 %s\n", argument.c_str(),
                    splat::Sha256::of_hex(text.value().data(), text.value().size()).c_str());
        return 0;
    }

    if (command == "fetch") {
        if (argument.empty()) {
            usage();
            return 2;
        }
        splat::Progress progress(progress_printer());
        const splat::Status fetched = manager.fetch(argument, progress);
        if (!fetched.ok()) {
            std::fprintf(stderr, "splat_model: %s\n", fetched.message().c_str());
            const splat::Result<splat::ModelRecord> record = manager.status(argument);
            if (record.ok() && !record.value().message.empty()) {
                std::fprintf(stderr, "  state: %s (%s)\n",
                             splat::model_state_name(record.value().state),
                             record.value().message.c_str());
            }
            return 1;
        }
        const splat::Result<splat::ModelRecord> record = manager.status(argument);
        if (record.ok()) print_record(record.value());
        return 0;
    }

    if (command == "import") {
        if (argument.empty()) {
            usage();
            return 2;
        }
        splat::Progress progress(progress_printer());
        const splat::Status imported = manager.import_file(argument, progress);
        if (!imported.ok()) {
            std::fprintf(stderr, "splat_model: %s\n", imported.message().c_str());
            return 1;
        }
        const std::string filename = splat::Path(argument).filename().string();
        for (const std::string& name : manager.backend_names()) {
            const splat::ModelBackendSpec* spec = manager.backend(name);
            if (spec == nullptr || spec->source_filename != filename) continue;
            const splat::Result<splat::ModelRecord> record = manager.status(name);
            if (record.ok()) print_record(record.value());
            break;
        }
        return 0;
    }

    std::fprintf(stderr, "splat_model: unknown command '%s'\n", command.c_str());
    usage();
    return 2;
}
