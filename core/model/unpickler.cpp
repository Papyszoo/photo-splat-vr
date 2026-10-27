#include "model/unpickler.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>

namespace splat {
namespace {

// ---- pickle opcodes the SHARP checkpoint uses -------------------------------
constexpr uint8_t kProto = 0x80;
constexpr uint8_t kGlobal = 'c';
constexpr uint8_t kEmptyTuple = ')';
constexpr uint8_t kEmptyDict = '}';  // protocol 1+; 'd' is the protocol-0 DICT
constexpr uint8_t kMark = '(';
constexpr uint8_t kTuple = 't';
constexpr uint8_t kTuple1 = 0x85;
constexpr uint8_t kTuple2 = 0x86;
constexpr uint8_t kTuple3 = 0x87;
constexpr uint8_t kReduce = 'R';
constexpr uint8_t kBinPersId = 'Q';
constexpr uint8_t kBinUnicode = 'X';
constexpr uint8_t kBinInt = 'J';
constexpr uint8_t kBinInt1 = 'K';
constexpr uint8_t kBinInt2 = 'M';
constexpr uint8_t kNewFalse = 0x89;
constexpr uint8_t kNewTrue = 0x88;
constexpr uint8_t kNone = 'N';
constexpr uint8_t kBinPut = 'q';
constexpr uint8_t kLongBinPut = 'r';
constexpr uint8_t kBinGet = 'h';
constexpr uint8_t kLongBinGet = 'j';
constexpr uint8_t kSetItem = 's';
constexpr uint8_t kSetItems = 'u';
constexpr uint8_t kBuild = 'b';
constexpr uint8_t kStop = '.';
constexpr uint8_t kStackGlobal = 0x93;

const char* opcode_name(uint8_t op) {
    switch (op) {
        case kProto: return "PROTO";
        case kGlobal: return "GLOBAL";
        case kEmptyTuple: return "EMPTY_TUPLE";
        case kEmptyDict: return "EMPTY_DICT";
        case kMark: return "MARK";
        case kTuple: return "TUPLE";
        case kTuple1: return "TUPLE1";
        case kTuple2: return "TUPLE2";
        case kTuple3: return "TUPLE3";
        case kReduce: return "REDUCE";
        case kBinPersId: return "BINPERSID";
        case kBinUnicode: return "BINUNICODE";
        case kBinInt: return "BININT";
        case kBinInt1: return "BININT1";
        case kBinInt2: return "BININT2";
        case kNewFalse: return "NEWFALSE";
        case kNewTrue: return "NEWTRUE";
        case kNone: return "NONE";
        case kBinPut: return "BINPUT";
        case kLongBinPut: return "LONG_BINPUT";
        case kBinGet: return "BINGET";
        case kLongBinGet: return "LONG_BINGET";
        case kSetItem: return "SETITEM";
        case kSetItems: return "SETITEMS";
        case kBuild: return "BUILD";
        case kStop: return "STOP";
        case kStackGlobal: return "STACK_GLOBAL";
        // Known pickle opcodes this parser deliberately does not implement.
        // Naming them makes a rejected checkpoint diagnosable.
        case 'G': return "BINFLOAT";
        case 'L': return "LONG";
        case 0x8a: return "LONG1";
        case 0x8b: return "LONG4";
        case 0x94: return "MEMOIZE";
        case 0x95: return "FRAME";
        case ']': return "EMPTY_LIST";
        case 'd': return "DICT";
        case 'l': return "LIST";
        case 0x8f: return "EMPTY_SET";
        case 0x90: return "ADDITEMS";
        case 'a': return "APPEND";
        case 'e': return "APPENDS";
        case '0': return "POP";
        case '1': return "POP_MARK";
        case '2': return "DUP";
        case 'p': return "PUT";
        case 'g': return "GET";
        case 'T': return "BINSTRING";
        case 'S': return "STRING";
        case 'V': return "UNICODE";
        case 'i': return "INT";
        case 'I': return "INT";
        case 'F': return "FLOAT";
        case 0x8c: return "SHORT_BINUNICODE";
        case 0x8d: return "BINUNICODE8";
        case 0x8e: return "BINBYTES8";
        case 'C': return "SHORT_BINBYTES";
        case 'B': return "BINBYTES";
        case 0x81: return "NEWOBJ";
        case 0x82: return "EXT1";
        case 0x92: return "NEWOBJ_EX";
        default: return "unknown";
    }
}

// ---- a tiny object model ----------------------------------------------------
struct Value;
using ValuePtr = std::shared_ptr<Value>;

enum class Kind { None, Bool, Int, String, Tuple, Dict, Global, Storage, Tensor };

struct Value {
    Kind kind = Kind::None;
    bool boolean = false;
    int64_t integer = 0;
    std::string text;  // String contents, "module.name" for Global, storage key for Storage
    std::vector<ValuePtr> items;                    // Tuple
    std::vector<std::pair<ValuePtr, ValuePtr>> map;  // Dict, insertion order kept
    StorageDtype storage_dtype = StorageDtype::Unsupported;
    uint64_t storage_numel = 0;
    size_t tensor_index = 0;  // index into PickleState::tensors for Kind::Tensor
};

ValuePtr make(Kind kind) {
    ValuePtr value = std::make_shared<Value>();
    value->kind = kind;
    return value;
}

ValuePtr make_string(std::string text) {
    ValuePtr value = make(Kind::String);
    value->text = std::move(text);
    return value;
}

ValuePtr make_int(int64_t integer) {
    ValuePtr value = make(Kind::Int);
    value->integer = integer;
    return value;
}

ValuePtr make_global(std::string module, std::string name) {
    ValuePtr value = make(Kind::Global);
    value->text = std::move(module) + "." + std::move(name);
    return value;
}

bool dtype_from_storage_global(const std::string& global, StorageDtype* dtype) {
    if (global == "torch.FloatStorage") {
        *dtype = StorageDtype::Float32;
        return true;
    }
    if (global == "torch.HalfStorage") {
        *dtype = StorageDtype::Float16;
        return true;
    }
    if (global == "torch.DoubleStorage") {
        *dtype = StorageDtype::Float64;
        return true;
    }
    return false;
}

// ---- the VM -----------------------------------------------------------------
class Vm {
public:
    Vm(const std::vector<uint8_t>& pickle, std::string origin)
        : data_(pickle.data()), size_(pickle.size()), origin_(std::move(origin)) {}

    Result<PickleState> run() {
        while (pos_ < size_) {
            if (!step()) return Result<PickleState>::failure(error_);
            if (stopped_) break;
        }
        if (!stopped_) {
            return Result<PickleState>::failure(origin_ + ": pickle ended without STOP at byte " +
                                                std::to_string(pos_));
        }
        if (state_.tensors.empty()) {
            return Result<PickleState>::failure(origin_ +
                                                ": pickle contains no tensor records");
        }
        for (const TensorRecord& tensor : state_.tensors) {
            if (tensor.name.empty()) {
                return Result<PickleState>::failure(
                    origin_ + ": tensor record for storage " + tensor.storage_key +
                    " never appeared in the state dict");
            }
        }
        return Result<PickleState>::success(std::move(state_));
    }

private:
    // --- primitives ---------------------------------------------------------
    bool need(size_t bytes) {
        if (size_ - pos_ < bytes) {
            return fail("truncated pickle: needed " + std::to_string(bytes) + " bytes at offset " +
                        std::to_string(pos_) + ", " + std::to_string(size_ - pos_) + " left");
        }
        return true;
    }

    bool read_u8(uint8_t* out) {
        if (!need(1)) return false;
        *out = data_[pos_++];
        return true;
    }

    bool read_le(size_t bytes, uint64_t* out) {
        if (!need(bytes)) return false;
        uint64_t value = 0;
        for (size_t i = 0; i < bytes; ++i) {
            value |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += bytes;
        *out = value;
        return true;
    }

    bool read_line(std::string* out) {
        const size_t start = pos_;
        while (pos_ < size_ && data_[pos_] != '\n') ++pos_;
        if (pos_ >= size_) return fail("unterminated GLOBAL line at byte " + std::to_string(start));
        out->assign(reinterpret_cast<const char*>(data_ + start), pos_ - start);
        ++pos_;
        return true;
    }

    bool pop(ValuePtr* out) {
        if (stack_.empty()) return fail("pickle stack underflow at byte " + std::to_string(pos_));
        *out = stack_.back();
        stack_.pop_back();
        return true;
    }

    bool top(size_t offset_from_top, ValuePtr* out) {
        if (stack_.size() <= offset_from_top) {
            return fail("pickle stack underflow at byte " + std::to_string(pos_));
        }
        *out = stack_[stack_.size() - 1 - offset_from_top];
        return true;
    }

    bool fail(std::string message) {
        if (error_.empty()) error_ = origin_ + ": " + std::move(message);
        return false;
    }

    bool require_kind(const ValuePtr& value, Kind kind, const char* what) {
        if (value->kind != kind) return fail(std::string("expected ") + what + " at byte " +
                                            std::to_string(pos_));
        return true;
    }

    // --- one opcode ---------------------------------------------------------
    bool step() {
        const size_t op_at = pos_;
        uint8_t op = 0;
        if (!read_u8(&op)) return false;

        switch (op) {
            case kProto: {
                uint8_t version = 0;
                if (!read_u8(&version)) return false;
                if (version > 2) {
                    return fail("unsupported pickle protocol " + std::to_string(version) +
                                " at byte " + std::to_string(op_at) + " (only protocol 2 is read)");
                }
                return true;
            }
            case kGlobal: {
                std::string module;
                std::string name;
                if (!read_line(&module) || !read_line(&name)) return false;
                return push_global(module, name);
            }
            case kStackGlobal: {
                ValuePtr name;
                ValuePtr module;
                if (!pop(&name) || !pop(&module)) return false;
                if (!require_kind(name, Kind::String, "STACK_GLOBAL name string")) return false;
                if (!require_kind(module, Kind::String, "STACK_GLOBAL module string")) return false;
                return push_global(module->text, name->text);
            }
            case kEmptyDict:
                stack_.push_back(make(Kind::Dict));
                return true;
            case kEmptyTuple:
                stack_.push_back(make(Kind::Tuple));
                return true;
            case kNone:
                stack_.push_back(make(Kind::None));
                return true;
            case kNewTrue:
                return push_bool(true);
            case kNewFalse:
                return push_bool(false);
            case kMark:
                marks_.push_back(stack_.size());
                return true;
            case kTuple:
            case kTuple1:
            case kTuple2:
            case kTuple3: {
                size_t count = 0;
                if (op == kTuple) {
                    if (marks_.empty()) return fail("TUPLE without MARK at byte " + std::to_string(op_at));
                    count = stack_.size() - marks_.back();
                } else {
                    count = static_cast<size_t>(op - kTuple1) + 1;
                }
                ValuePtr tuple = make(Kind::Tuple);
                if (count > stack_.size()) return fail("TUPLE underflow at byte " + std::to_string(op_at));
                tuple->items.assign(stack_.end() - static_cast<std::ptrdiff_t>(count), stack_.end());
                stack_.resize(stack_.size() - count);
                if (op == kTuple) marks_.pop_back();
                stack_.push_back(std::move(tuple));
                return true;
            }
            case kBinUnicode: {
                uint64_t length = 0;
                if (!read_le(4, &length)) return false;
                if (length > 1u << 20) {
                    return fail("BINUNICODE of " + std::to_string(length) +
                                " bytes at byte " + std::to_string(op_at) + " is implausible");
                }
                if (!need(static_cast<size_t>(length))) return false;
                std::string text(reinterpret_cast<const char*>(data_ + pos_),
                                 static_cast<size_t>(length));
                pos_ += static_cast<size_t>(length);
                stack_.push_back(make_string(std::move(text)));
                return true;
            }
            case kBinInt:
            case kBinInt1:
            case kBinInt2: {
                const size_t bytes = op == kBinInt ? 4 : (op == kBinInt1 ? 1 : 2);
                uint64_t raw = 0;
                if (!read_le(bytes, &raw)) return false;
                int64_t value = static_cast<int64_t>(raw);
                if (op == kBinInt) {  // sign extend the 32-bit form
                    value = static_cast<int32_t>(raw);
                }
                stack_.push_back(make_int(value));
                return true;
            }
            case kBinPut:
            case kLongBinPut: {
                const size_t bytes = op == kBinPut ? 1 : 4;
                uint64_t index = 0;
                if (!read_le(bytes, &index)) return false;
                ValuePtr value;
                if (!top(0, &value)) return false;
                memo_[static_cast<int64_t>(index)] = value;
                return true;
            }
            case kBinGet:
            case kLongBinGet: {
                const size_t bytes = op == kBinGet ? 1 : 4;
                uint64_t index = 0;
                if (!read_le(bytes, &index)) return false;
                const auto found = memo_.find(static_cast<int64_t>(index));
                if (found == memo_.end()) {
                    return fail("BINGET of unset memo slot " + std::to_string(index) + " at byte " +
                                std::to_string(op_at));
                }
                stack_.push_back(found->second);
                return true;
            }
            case kReduce:
                return reduce(op_at);
            case kBinPersId:
                return persistent_id(op_at);
            case kSetItem:
                return set_item(op_at);
            case kSetItems:
                return set_items(op_at);
            case kBuild:
                return build(op_at);
            case kStop:
                stopped_ = true;
                return true;
            default:
                return fail("unsupported pickle opcode 0x" + hex_byte(op) + " (" +
                            opcode_name(op) + ") at byte " + std::to_string(op_at) +
                            "; only the opcodes used by the SHARP checkpoint are read");
        }
    }

    static std::string hex_byte(uint8_t value) {
        static const char kHex[] = "0123456789abcdef";
        std::string text;
        text.push_back(kHex[value >> 4]);
        text.push_back(kHex[value & 0x0f]);
        return text;
    }

    bool push_bool(bool value) {
        ValuePtr item = make(Kind::Bool);
        item->boolean = value;
        stack_.push_back(std::move(item));
        return true;
    }

    bool push_global(const std::string& module, const std::string& name) {
        const std::string full = module + "." + name;
        const bool allowed = full == "collections.OrderedDict" ||
                             full == "torch._utils._rebuild_tensor_v2" ||
                             full == "torch.FloatStorage" || full == "torch.HalfStorage" ||
                             full == "torch.DoubleStorage";
        if (!allowed) {
            return fail("unsupported global '" + full + "' at byte " + std::to_string(pos_) +
                        "; allowed globals are collections.OrderedDict, "
                        "torch._utils._rebuild_tensor_v2 and torch.FloatStorage");
        }
        stack_.push_back(make_global(module, name));
        return true;
    }

    bool reduce(size_t op_at) {
        ValuePtr args;
        ValuePtr callable;
        if (!pop(&args) || !pop(&callable)) return false;
        if (!require_kind(callable, Kind::Global, "REDUCE callable")) return false;
        if (!require_kind(args, Kind::Tuple, "REDUCE argument tuple")) return false;

        if (callable->text == "collections.OrderedDict") {
            if (!args->items.empty()) {
                return fail("OrderedDict() with arguments at byte " + std::to_string(op_at) +
                            "; only the empty form is read");
            }
            stack_.push_back(make(Kind::Dict));
            return true;
        }

        if (callable->text == "torch._utils._rebuild_tensor_v2") {
            return rebuild_tensor(args, op_at);
        }

        return fail("REDUCE on '" + callable->text + "' at byte " + std::to_string(op_at) +
                    " is not allowed; only collections.OrderedDict() and "
                    "torch._utils._rebuild_tensor_v2 are evaluated");
    }

    bool rebuild_tensor(const ValuePtr& args, size_t op_at) {
        if (args->items.size() != 6) {
            return fail("_rebuild_tensor_v2 with " + std::to_string(args->items.size()) +
                        " arguments at byte " + std::to_string(op_at) + ", expected 6");
        }
        const ValuePtr& storage = args->items[0];
        if (!require_kind(storage, Kind::Storage, "storage persist-id")) return false;
        if (!require_kind(args->items[1], Kind::Int, "storage offset")) return false;
        if (!require_kind(args->items[2], Kind::Tuple, "size tuple")) return false;
        if (!require_kind(args->items[3], Kind::Tuple, "stride tuple")) return false;

        TensorRecord record;
        record.storage_key = storage->text;
        record.dtype = storage->storage_dtype;
        record.storage_numel = storage->storage_numel;
        record.storage_offset = args->items[1]->integer;

        for (const ValuePtr& item : args->items[2]->items) {
            if (!require_kind(item, Kind::Int, "size entry")) return false;
            if (item->integer < 0) return fail("negative tensor dimension at byte " + std::to_string(op_at));
            record.shape.push_back(item->integer);
        }
        for (const ValuePtr& item : args->items[3]->items) {
            if (!require_kind(item, Kind::Int, "stride entry")) return false;
            record.stride.push_back(item->integer);
        }
        if (record.shape.empty() || record.shape.size() != record.stride.size()) {
            return fail("tensor size/stride rank mismatch at byte " + std::to_string(op_at));
        }

        state_.tensors.push_back(std::move(record));
        ValuePtr tensor = make(Kind::Tensor);
        tensor->tensor_index = state_.tensors.size() - 1;
        stack_.push_back(std::move(tensor));
        return true;
    }

    bool persistent_id(size_t op_at) {
        ValuePtr value;
        if (!pop(&value)) return false;
        if (!require_kind(value, Kind::Tuple, "BINPERSID tuple")) return false;
        const std::vector<ValuePtr>& items = value->items;
        if (items.size() != 5) {
            return fail("BINPERSID with " + std::to_string(items.size()) +
                        " fields at byte " + std::to_string(op_at) +
                        ", expected ('storage', StorageType, key, 'cpu', numel)");
        }
        if (!require_kind(items[0], Kind::String, "BINPERSID 'storage' tag")) return false;
        if (items[0]->text != "storage") {
            return fail("BINPERSID tag '" + items[0]->text + "' at byte " + std::to_string(op_at) +
                        " is unsupported");
        }
        if (!require_kind(items[1], Kind::Global, "storage type")) return false;
        if (!require_kind(items[2], Kind::String, "storage key")) return false;
        if (!require_kind(items[3], Kind::String, "storage device")) return false;
        if (!require_kind(items[4], Kind::Int, "storage size")) return false;

        StorageDtype dtype = StorageDtype::Unsupported;
        if (!dtype_from_storage_global(items[1]->text, &dtype)) {
            return fail("storage type '" + items[1]->text + "' at byte " + std::to_string(op_at) +
                        " is unsupported; only float32 tensors are converted");
        }
        if (items[3]->text != "cpu") {
            return fail("storage device '" + items[3]->text + "' at byte " + std::to_string(op_at) +
                        " is unsupported; only cpu storages are read");
        }
        if (items[4]->integer < 0) {
            return fail("negative storage element count at byte " + std::to_string(op_at));
        }

        ValuePtr storage_value = make(Kind::Storage);
        storage_value->text = items[2]->text;
        storage_value->storage_dtype = dtype;
        storage_value->storage_numel = static_cast<uint64_t>(items[4]->integer);
        stack_.push_back(std::move(storage_value));
        return true;
    }

    bool set_item(size_t op_at) {
        ValuePtr value;
        ValuePtr key;
        ValuePtr target;
        if (!pop(&value) || !pop(&key) || !top(0, &target)) return false;
        if (!require_kind(target, Kind::Dict, "SETITEM target dict")) return false;
        return assign(target, key, value, op_at);
    }

    bool set_items(size_t op_at) {
        if (marks_.empty()) return fail("SETITEMS without MARK at byte " + std::to_string(op_at));
        const size_t mark = marks_.back();
        marks_.pop_back();
        if (mark < 1 || mark - 1 >= stack_.size()) {
            return fail("SETITEMS underflow at byte " + std::to_string(op_at));
        }
        ValuePtr target = stack_[mark - 1];
        if (!require_kind(target, Kind::Dict, "SETITEMS target dict")) return false;
        const size_t pairs = stack_.size() - mark;
        if (pairs % 2 != 0) return fail("SETITEMS with an odd number of items at byte " + std::to_string(op_at));
        for (size_t i = mark; i + 1 < stack_.size(); i += 2) {
            if (!assign(target, stack_[i], stack_[i + 1], op_at)) return false;
        }
        // The dict stays on the stack: CPython keeps it and starts the next
        // batch with a bare MARK.
        stack_.resize(mark);
        return true;
    }

    bool assign(const ValuePtr& dict, const ValuePtr& key, const ValuePtr& value, size_t op_at) {
        if (key->kind != Kind::String) {
            return fail("dictionary key is not a string at byte " + std::to_string(op_at) +
                        "; only state dicts are read");
        }
        if (value->kind == Kind::Tensor) {
            state_.tensors[value->tensor_index].name = key->text;
        }
        // Ordered dict semantics: setting an existing key replaces its value and
        // keeps its position.
        for (auto& entry : dict->map) {
            if (entry.first->text == key->text) {
                entry.second = value;
                return true;
            }
        }
        dict->map.emplace_back(key, value);
        return true;
    }

    bool build(size_t op_at) {
        ValuePtr state;
        ValuePtr target;
        if (!pop(&state) || !top(0, &target)) return false;
        if (target->kind == Kind::Dict && state->kind == Kind::Dict) {
            for (const auto& pair : state->map) {
                if (pair.first->kind != Kind::String) {
                    return fail("BUILD state key is not a string at byte " + std::to_string(op_at));
                }
                target->map.emplace_back(pair.first, pair.second);
            }
            return true;
        }
        if (target->kind == Kind::Dict && state->kind == Kind::None) return true;
        return fail("BUILD on an unsupported object at byte " + std::to_string(op_at) +
                    "; only dict state (torch _metadata) is read");
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    std::string origin_;
    std::string error_;
    bool stopped_ = false;
    std::vector<ValuePtr> stack_;
    std::vector<size_t> marks_;
    std::unordered_map<int64_t, ValuePtr> memo_;
    PickleState state_;
};

}  // namespace

uint64_t TensorRecord::numel() const {
    uint64_t count = 1;
    for (int64_t dim : shape) {
        // A crafted pickle can carry dimensions that overflow the product; the
        // caller treats kInvalidNumel as a hard error instead of wrapping into a
        // small, plausible-looking element count.
        if (dim < 0) return kInvalidNumel;
        const uint64_t size = static_cast<uint64_t>(dim);
        if (size != 0 && count > kInvalidNumel / size) return kInvalidNumel;
        count *= size;
    }
    return count;
}

bool TensorRecord::contiguous() const {
    int64_t expected = 1;
    for (size_t i = shape.size(); i-- > 0;) {
        if (shape[i] < 0) return false;
        if (shape[i] == 0) continue;
        if (stride[i] != expected) return false;
        if (expected > INT64_MAX / shape[i]) return false;
        expected *= shape[i];
    }
    return true;
}

size_t storage_itemsize(StorageDtype dtype) {
    switch (dtype) {
        case StorageDtype::Float32: return 4;
        case StorageDtype::Float16: return 2;
        case StorageDtype::Float64: return 8;
        case StorageDtype::Unsupported: return 0;
    }
    return 0;
}

Result<PickleState> unpickle_state_dict(const std::vector<uint8_t>& pickle,
                                       const std::string& origin) {
    Vm vm(pickle, origin);
    return vm.run();
}

}  // namespace splat
