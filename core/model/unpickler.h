#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/status.h"

namespace splat {

// Storage dtypes a torch.save checkpoint can reference. Only Float32 is
// converted; the others are recognised so the error names them.
enum class StorageDtype { Float32, Float16, Float64, Unsupported };

// One `torch._utils._rebuild_tensor_v2` record from data.pkl.
struct TensorRecord {
    std::string name;         // state-dict key, e.g. "feature_model.decoder.convs.1.weight"
    std::string storage_key;  // zip entry "<prefix>/data/<storage_key>"
    StorageDtype dtype = StorageDtype::Unsupported;
    std::vector<int64_t> shape;   // torch order, outermost dimension first
    std::vector<int64_t> stride;  // same order as shape
    int64_t storage_offset = 0;
    uint64_t storage_numel = 0;

    // Returned by numel() when a dimension is negative or the product of the
    // dimensions does not fit in 64 bits. Callers must reject it.
    static constexpr uint64_t kInvalidNumel = ~uint64_t{0};

    uint64_t numel() const;
    bool contiguous() const;
};

struct PickleState {
    std::vector<TensorRecord> tensors;  // in state-dict order
};

// Decodes the pickled state dict of a torch.save zipfile checkpoint.
//
// Deliberately not a Python pickle: only the opcodes and the four globals that
// the SHARP checkpoint uses are implemented. Any other opcode or global is an
// error, so there is no arbitrary code path (spec C1, "minimal unpickler").
// `origin` is used in error messages only.
Result<PickleState> unpickle_state_dict(const std::vector<uint8_t>& pickle, const std::string& origin);

// Size in bytes of one tensor element for its storage dtype, or 0 if unknown.
size_t storage_itemsize(StorageDtype dtype);

}  // namespace splat
