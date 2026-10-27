#pragma once

// Generic .npy reader and tensor comparator for the parity tests.
//
// Header-only on purpose: the parity tests live in tests/sharp_*_test.cpp and
// the build wires tests from a glob, so a new translation unit here would need
// CMake edits in two places. Everything is `inline`.
//
// The reference dumps are PyTorch CPU tensors written with numpy.save: C order,
// little-endian float32 (or uint8 for the decoded image). ggml stores ne0
// fastest, which is the transpose of the torch shape, but *both* are row-major
// over the same linear element order, so a dump and a ggml tensor can be
// compared element by element; only the shapes have to be checked against each
// other.
//
//   torch (1, 256, 768, 768)  <->  ggml ne (768, 768, 256, 1)   same order
//   torch (1, 577, 1024)      <->  ggml ne (1024, 577, 1, 1)    same order
//
// A few dumps are Fortran order: numpy writes F when the tensor handed to it is
// not C-contiguous, which is what the composer's `.permute(...).flatten(...)`
// outputs are. Those are read as the same logical tensor (the reader transposes
// them back), so the comparison above still holds.
//
// Percentiles are estimated from a log-spaced histogram of the relative error
// (one bin per 0.01 decade, upper edge reported, so a value is at most 2.3%
// pessimistic). That keeps the 604 MiB level-0 tensor comparable in constant
// memory; max absolute and max relative error are exact.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace splat {
namespace parity {

// Relative error is only meaningful away from zero; this is the same threshold
// the DA2 parity test uses and docs/research/sharp-reference.md §10 motivates.
inline constexpr double kRelativeFloor = 1e-2;

struct NpyHeader {
    std::vector<int64_t> shape;  // as written, outermost first
    size_t elements = 0;
    size_t data_offset = 0;
    char kind = 'f';             // 'f' = float32, 'u' = uint8
    bool fortran = false;
};

// Reads and validates the header; leaves the payload at data_offset.
inline bool read_npy_header(const std::string& path, NpyHeader* header, std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "cannot open " + path;
        return false;
    }
    char magic[6] = {};
    stream.read(magic, 6);
    if (std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        *error = path + ": not a .npy file";
        return false;
    }
    unsigned char version[2] = {};
    stream.read(reinterpret_cast<char*>(version), 2);
    uint32_t header_length = 0;
    if (version[0] == 1) {
        unsigned char raw[2] = {};
        stream.read(reinterpret_cast<char*>(raw), 2);
        header_length = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8);
    } else {
        unsigned char raw[4] = {};
        stream.read(reinterpret_cast<char*>(raw), 4);
        header_length = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8) |
                        (static_cast<uint32_t>(raw[2]) << 16) |
                        (static_cast<uint32_t>(raw[3]) << 24);
    }
    std::string text(header_length, '\0');
    stream.read(text.data(), header_length);

    if (text.find("'fortran_order': True") != std::string::npos ||
        text.find("\"fortran_order\": true") != std::string::npos) {
        header->fortran = true;
    } else {
        header->fortran = false;
    }
    if (text.find("'<f4'") != std::string::npos) {
        header->kind = 'f';
    } else if (text.find("'|u1'") != std::string::npos) {
        header->kind = 'u';
    } else {
        *error = path + ": only little-endian float32 and uint8 arrays are read";
        return false;
    }
    const size_t shape_at = text.find("'shape': (");
    if (shape_at == std::string::npos) {
        *error = path + ": no shape in the header";
        return false;
    }
    const size_t close = text.find(')', shape_at);
    if (close == std::string::npos || close < shape_at + 10) {
        *error = path + ": unterminated shape in the header";
        return false;
    }
    const std::string shape_text = text.substr(shape_at + 10, close - shape_at - 10);
    std::vector<int64_t> shape;
    std::string number;
    // A truncated or hand-edited header must fail here with a message instead of
    // throwing out of std::stoll.
    auto append_number = [&]() -> bool {
        if (number.empty()) return true;
        try {
            shape.push_back(std::stoll(number));
        } catch (const std::exception&) {
            *error = path + ": unreadable shape value '" + number + "'";
            return false;
        }
        number.clear();
        return true;
    };
    for (char character : shape_text) {
        if (character == ',' || character == ' ') {
            if (!append_number()) return false;
        } else if (character >= '0' && character <= '9') {
            number.push_back(character);
        } else {
            *error = path + ": malformed shape in the header";
            return false;
        }
    }
    if (!append_number()) return false;
    size_t elements = 1;
    for (int64_t dimension : shape) elements *= static_cast<size_t>(dimension);
    header->shape = std::move(shape);
    header->elements = elements;
    header->data_offset = 6 + 2 + (version[0] == 1 ? 2 : 4) + header_length;
    return true;
}

inline std::string shape_text(const std::vector<int64_t>& shape) {
    std::string text = "(";
    for (size_t index = 0; index < shape.size(); ++index) {
        if (index != 0) text += ", ";
        text += std::to_string(shape[index]);
    }
    if (shape.size() == 1) text += ",";
    return text + ")";
}

// Drops the leading unit dimensions of a torch shape and the trailing unit
// dimensions of a ggml ne vector, then requires the rest to agree. Both end up
// describing the same data in the same element order.
//
// Only the *leading* unit dims of the torch shape are dropped, so a dump with
// interior unit dims (SHARP's `051_init_base_quaternion.npy` is
// (1, 4, 1, 1, 1)) cannot be expressed: compare those directly.
inline bool same_layout(const std::vector<int64_t>& npy_shape, const std::vector<int64_t>& ne,
                        std::string* error) {
    std::vector<int64_t> torch_shape = npy_shape;
    while (torch_shape.size() > 1 && torch_shape.front() == 1) torch_shape.erase(torch_shape.begin());
    std::vector<int64_t> ggml_shape;
    for (size_t index = 0; index < ne.size(); ++index) {
        ggml_shape.push_back(ne[ne.size() - 1 - index]);
    }
    while (!ggml_shape.empty() && ggml_shape.back() == 1) ggml_shape.pop_back();
    while (!ggml_shape.empty() && ggml_shape.front() == 1) ggml_shape.erase(ggml_shape.begin());
    while (torch_shape.size() < ggml_shape.size()) torch_shape.insert(torch_shape.begin(), 1);
    while (ggml_shape.size() < torch_shape.size()) ggml_shape.insert(ggml_shape.begin(), 1);
    if (torch_shape != ggml_shape) {
        *error = "shape mismatch: golden " + shape_text(npy_shape) + " vs ggml " + shape_text(ne);
        return false;
    }
    return true;
}

struct Stats {
    bool ok = false;
    std::string error;
    size_t compared = 0;
    double max_abs = 0.0;
    double max_rel = 0.0;
    double p99_rel = 0.0;
    double p999_rel = 0.0;
    double cosine = 1.0;
};

// Log-spaced relative-error histogram: 0.01 decade per bin from 1e-15 to 1e5.
class RelHistogram {
public:
    RelHistogram() : counts_(kBins, 0) {}

    void add(double relative) {
        int bin = 0;
        if (relative > 0.0) {
            const double exponent = std::log10(relative);
            bin = static_cast<int>((exponent - kLow) / kStep);
            bin = std::min(std::max(bin, 0), kBins - 1);
        }
        ++counts_[static_cast<size_t>(bin)];
        ++total_;
    }

    // Upper edge of the bin holding the requested quantile.
    double quantile(double fraction) const {
        if (total_ == 0) return 0.0;
        const size_t target = static_cast<size_t>(fraction * static_cast<double>(total_ - 1));
        size_t seen = 0;
        for (int bin = 0; bin < kBins; ++bin) {
            seen += counts_[static_cast<size_t>(bin)];
            if (seen > target) return std::pow(10.0, kLow + (bin + 1) * kStep);
        }
        return std::pow(10.0, kLow + kBins * kStep);
    }

private:
    static constexpr double kLow = -15.0;
    static constexpr double kStep = 0.01;
    static constexpr int kBins = 2000;
    std::vector<uint64_t> counts_;
    size_t total_ = 0;
};

// Compares a float32 .npy dump against `values` (ggml order) in fixed-size
// blocks, so a 604 MiB stage costs one block of host memory on top of `values`.
// A Fortran-order dump is the same logical tensor stored back to front; the
// comparison walks the stored order with an odometer and indexes the candidate
// in C order, so it stays streaming.
//
// `max_reference` (infinite by default) restricts the comparison to elements
// whose reference magnitude is at most that value. It exists for tensors with a
// clamp in the middle: monodepth is `disparity_factor / clamp(disparity, 1e-4,
// 1e4)`, so the ~3% of sky pixels that sit on the 1e-4 clamp produce depths near
// 8000 m where a one-quantum disparity difference is a huge relative error. The
// initialiser clamps those away (`depth * factor, max=100`), so the gate is
// taken over the range that survives; the full-range numbers are still printed.
inline Stats compare_npy(const std::string& path, const std::vector<int64_t>& ne, const float* values,
                         double max_reference = std::numeric_limits<double>::infinity()) {
    Stats stats;
    NpyHeader header;
    if (!read_npy_header(path, &header, &stats.error)) return stats;
    if (header.kind != 'f') {
        stats.error = path + ": expected a float32 array";
        return stats;
    }
    if (!same_layout(header.shape, ne, &stats.error)) return stats;
    const size_t elements = header.elements;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        stats.error = "cannot reopen " + path;
        return stats;
    }
    stream.seekg(static_cast<std::streamoff>(header.data_offset));

    // One pass over the reference's stored order, accumulating against the
    // candidate in C order. For a C-order dump the two indexes are equal.
    std::vector<size_t> c_stride(header.shape.size());
    {
        size_t stride = 1;
        for (size_t index = header.shape.size(); index-- > 0;) {
            c_stride[index] = stride;
            stride *= static_cast<size_t>(header.shape[index]);
        }
    }
    std::vector<int64_t> cursor(header.shape.size(), 0);
    // Fortran order increments the first axis fastest.
    auto advance = [&]() {
        for (size_t index = 0; index < header.shape.size(); ++index) {
            if (++cursor[index] < header.shape[index]) return;
            cursor[index] = 0;
        }
    };
    auto c_index = [&]() {
        size_t index = 0;
        for (size_t axis = 0; axis < cursor.size(); ++axis) {
            index += static_cast<size_t>(cursor[axis]) * c_stride[axis];
        }
        return index;
    };

    class Accumulator {
    public:
        void add(double candidate, double reference) {
            const double difference = std::fabs(candidate - reference);
            max_abs = std::max(max_abs, difference);
            dot += candidate * reference;
            norm_a += candidate * candidate;
            norm_b += reference * reference;
            if (std::fabs(reference) > kRelativeFloor) {
                const double rel = difference / std::fabs(reference);
                max_rel = std::max(max_rel, rel);
                relative.add(rel);
            }
        }
        double max_abs = 0.0;
        double max_rel = 0.0;
        double dot = 0.0;
        double norm_a = 0.0;
        double norm_b = 0.0;
        RelHistogram relative;
    } accumulator;

    constexpr size_t kBlock = 1u << 20;  // 4 MiB of floats
    std::vector<float> expected(kBlock);
    size_t compared = 0;
    for (size_t offset = 0; offset < elements; offset += kBlock) {
        const size_t count = std::min(kBlock, elements - offset);
        stream.read(reinterpret_cast<char*>(expected.data()),
                    static_cast<std::streamsize>(count * sizeof(float)));
        if (static_cast<size_t>(stream.gcount()) != count * sizeof(float)) {
            stats.error = path + ": truncated payload";
            return stats;
        }
        for (size_t index = 0; index < count; ++index) {
            const double reference = expected[index];
            const size_t candidate_index = header.fortran ? c_index() : offset + index;
            if (std::fabs(reference) <= max_reference) {
                accumulator.add(values[candidate_index], reference);
                ++compared;
            }
            if (header.fortran) advance();
        }
    }
    if (compared == 0) {
        stats.error = path + ": no element survived the reference-range filter";
        return stats;
    }
    stats.compared = compared;
    stats.max_abs = accumulator.max_abs;
    stats.max_rel = accumulator.max_rel;
    stats.p99_rel = accumulator.relative.quantile(0.99);
    stats.p999_rel = accumulator.relative.quantile(0.999);
    const double denominator = std::sqrt(accumulator.norm_a) * std::sqrt(accumulator.norm_b);
    stats.cosine = denominator > 0.0 ? accumulator.dot / denominator : 1.0;
    stats.ok = true;
    return stats;
}

// Reads a float32 .npy array into `out` in C order (a Fortran-order dump is
// transposed back). Used by the parity tests that feed a reference tensor into
// a stage instead of a stage output.
inline bool read_npy_f32(const std::string& path, std::vector<int64_t>* shape,
                         std::vector<float>* out, std::string* error) {
    NpyHeader header;
    if (!read_npy_header(path, &header, error)) return false;
    if (header.kind != 'f') {
        *error = path + ": expected a float32 array";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "cannot reopen " + path;
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(header.data_offset));
    out->resize(header.elements);
    stream.read(reinterpret_cast<char*>(out->data()),
                static_cast<std::streamsize>(header.elements * sizeof(float)));
    if (!stream) {
        *error = path + ": truncated payload";
        return false;
    }
    if (header.fortran) {
        std::vector<size_t> c_stride(header.shape.size());
        size_t stride = 1;
        for (size_t index = header.shape.size(); index-- > 0;) {
            c_stride[index] = stride;
            stride *= static_cast<size_t>(header.shape[index]);
        }
        std::vector<float> ordered(header.elements);
        std::vector<int64_t> cursor(header.shape.size(), 0);
        for (size_t index = 0; index < header.elements; ++index) {
            size_t c_index = 0;
            for (size_t axis = 0; axis < cursor.size(); ++axis) {
                c_index += static_cast<size_t>(cursor[axis]) * c_stride[axis];
            }
            ordered[c_index] = (*out)[index];
            for (size_t axis = 0; axis < header.shape.size(); ++axis) {
                if (++cursor[axis] < header.shape[axis]) break;
                cursor[axis] = 0;
            }
        }
        *out = std::move(ordered);
    }
    *shape = header.shape;
    return true;
}

// Reads a uint8 .npy array (the decoded reference image) into `out`.
inline bool read_npy_u8(const std::string& path, std::vector<int64_t>* shape,
                        std::vector<uint8_t>* out, std::string* error) {
    NpyHeader header;
    if (!read_npy_header(path, &header, error)) return false;
    if (header.kind != 'u') {
        *error = path + ": expected a uint8 array";
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "cannot reopen " + path;
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(header.data_offset));
    out->resize(header.elements);
    stream.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(header.elements));
    if (!stream) {
        *error = path + ": truncated payload";
        return false;
    }
    *shape = header.shape;
    return true;
}

}  // namespace parity
}  // namespace splat
