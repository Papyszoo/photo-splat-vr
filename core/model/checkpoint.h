#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/path.h"
#include "common/status.h"
#include "model/unpickler.h"
#include "model/zip_archive.h"

namespace splat {

// The official checkpoint (spec rule 2): pinned identity, verified before every
// conversion. ml-sharp README URL:
//   https://ml-site.cdn-apple.com/models/sharp/sharp_2572gikvuh.pt
inline constexpr char kSharpCheckpointName[] = "sharp_2572gikvuh.pt";
inline constexpr char kSharpCheckpointSha256[] =
    "94211a75198c47f61fca7d739ba08a215418d8d398d48fddf023baccc24f073d";
inline constexpr uint64_t kSharpCheckpointBytes = 2809738232ull;
inline constexpr uint32_t kSharpCheckpointTensorCount = 1038;
inline constexpr uint64_t kSharpCheckpointTensorBytes = 2809220676ull;

// One tensor of the checkpoint: the pickle record augmented with the zip entry
// that holds its bytes.
struct CheckpointTensor {
    TensorRecord record;
    std::string zip_name;  // "<prefix>/data/<storage key>"
};

struct Checkpoint {
    Path path;
    std::string prefix;  // archive folder, e.g. "2572gikvuh_60000_lama/"
    bool zip64 = false;
    std::string byteorder;  // "little"
    std::vector<CheckpointTensor> tensors;
    ZipArchive zip;
};

// Opens the zip, reads its (small) pickle and cross-checks every tensor record
// against its blob: contiguous strides, float32 storage, offset inside the
// storage, and a zip entry of exactly numel x 4 bytes.
Result<Checkpoint> open_checkpoint(const Path& path);

// Streams one tensor's float32 payload through `sink`, skipping any storage
// offset. Data arrives in bounded chunks; nothing larger than one chunk is
// buffered.
Status read_checkpoint_tensor(ZipArchive& zip, const CheckpointTensor& tensor, const ZipSink& sink);

}  // namespace splat
