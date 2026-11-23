#!/usr/bin/env python3
"""Generates the synthetic checkpoint fixture for the C1 converter tests.

    DATA=${SPLAT_DATA_DIR:-$(cd .. && pwd)}   # the repo parent by default
    "$DATA"/ref/venv-sharp/bin/python tests/fixtures/model/make_fixture.py

Writes two files next to this script (tests/fixtures/model/):

  synthetic.pt    a real torch.save zipfile (protocol 2, one storage per tensor),
                  so the C++ side sees the same opcodes, globals and storage
                  layout as the Apple checkpoint, with a few kilobytes of data
  synthetic.json  the expected tensor table: names, shapes, dtypes and a few
                  values per tensor, read straight out of PyTorch

Shapes and names follow the real state dict closely, including tensors the
converter must drop (the two ViT classifier heads and the depth_adjustment
U-Net) and 1-D tensors that must stay float32.

torch.save writes the data/<key> entries in creation (numeric) order; the
Apple checkpoint has them in sorted-key order, which is the only order the
streaming converter can predict from data.pkl alone. The archive is therefore
rewritten in place after torch.save() (reorder_storage_entries): the same local
header + payload + descriptor blocks, sorted, with a rebuilt central directory.

    python3 tests/fixtures/model/make_fixture.py --reorder-only

re-applies that reordering and updates the digest in synthetic.json without
torch.
"""
import collections
import hashlib
import json
import os
import struct
import sys

LOCAL_SIG = b"PK\x03\x04"
CENTRAL_SIG = b"PK\x01\x02"
END_SIG = b"PK\x05\x06"
ZIP64_END_SIG = b"PK\x06\x06"

# The script lives beside its output, so it regenerates the fixture in place.
OUT_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_PT = os.path.join(OUT_DIR, "synthetic.pt")
OUT_JSON = os.path.join(OUT_DIR, "synthetic.json")

TORCH_PREFIX = "monodepth_model.monodepth_predictor."

# Values chosen to cover fp16 rounding: exact halves, a third, a large value,
# a subnormal, negatives.
FP16_PROBES = [0.0, 1.0, -1.0, 0.5, 1.0 / 3.0, 2.5, -2.5, 65504.0, 1e-5, 6.103515625e-05,
               1234.5, -0.001, 1e-8, 0.1, 0.2, 0.3]


def probe(shape, offset=0):
    count = 1
    for dim in shape:
        count *= dim
    values = [FP16_PROBES[(i + offset) % len(FP16_PROBES)] + i for i in range(count)]
    return torch.tensor(values, dtype=torch.float32).reshape(shape)


def gguf_name(name):
    return name[len(TORCH_PREFIX):] if name.startswith(TORCH_PREFIX) else name


def spot_indices(count):
    wanted = {0, 1, 2, count // 2, count - 2, count - 1}
    return [i for i in sorted(wanted) if 0 <= i < count]


def manifest(state, dropped):
    tensors = []
    for name, tensor in state.items():
        if name in dropped:
            continue
        flat = tensor.reshape(-1)
        indices = spot_indices(flat.numel())
        tensors.append({
            "name": name,
            "gguf_name": gguf_name(name),
            "shape": list(tensor.shape),
            "dtype": "float16" if tensor.dim() >= 2 else "float32",
            "spot_indices": indices,
            "spot_values": [float(flat[i]) for i in indices],
        })
    return tensors


def reorder_storage_entries(path):
    """Sorts the data/<key> entries of a torch.save archive by key, in place.

    Only the local header + payload + descriptor blocks are copied; the central
    directory is rebuilt with the new offsets. The archive's total size and the
    blocks themselves do not change, so the records after the central directory
    (zip64 end + locator + end) are copied verbatim.
    """
    data = open(path, "rb").read()
    end = data.rfind(END_SIG)
    zip64_end = data.rfind(ZIP64_END_SIG)
    count = struct.unpack_from("<H", data, end + 10)[0]
    if zip64_end >= 0:
        cd_offset = struct.unpack_from("<Q", data, zip64_end + 48)[0]
    else:
        cd_offset = struct.unpack_from("<I", data, end + 16)[0]

    entries = []
    at = cd_offset
    for _ in range(count):
        fields = struct.unpack_from("<IHHHHHHIIIHHHHHII", data, at)
        flags, name_len, extra_len, comment_len = fields[3], fields[10], fields[11], fields[12]
        record_end = at + 46 + name_len + extra_len + comment_len
        name = data[at + 46:at + 46 + name_len].decode()
        local = fields[16]
        local_name, local_extra = struct.unpack_from("<HH", data, local + 26)
        payload = fields[8]  # stored entries: compressed size == payload size
        block_end = local + 30 + local_name + local_extra + payload
        if flags & 0x8:  # a 16-byte data descriptor follows the payload
            block_end += 16
        entries.append({"name": name, "record": data[at:record_end], "block": data[local:block_end]})
        at = record_end

    keys = [i for i, entry in enumerate(entries) if "/data/" in entry["name"]]
    if keys:
        first, last = keys[0], keys[-1]
        ordered = (entries[:first] +
                   sorted(entries[first:last + 1], key=lambda entry: entry["name"].rsplit("/", 1)[-1]) +
                   entries[last + 1:])
    else:
        ordered = entries

    out = bytearray()
    records = bytearray()
    for entry in ordered:
        record = bytearray(entry["record"])
        struct.pack_into("<I", record, 42, len(out))  # relative offset of the local header
        out += entry["block"]
        records += record
    out += records
    out += data[cd_offset + sum(len(e["record"]) for e in entries):]

    with open(path, "wb") as handle:
        handle.write(out)
    return len(out)


def update_manifest_digest():
    size = os.path.getsize(OUT_PT)
    digest = hashlib.sha256(open(OUT_PT, "rb").read()).hexdigest()
    with open(OUT_JSON) as handle:
        document = json.load(handle)
    document["checkpoint_sha256"] = digest
    document["checkpoint_bytes"] = size
    with open(OUT_JSON, "w") as handle:
        json.dump(document, handle, indent=1)
        handle.write("\n")
    return digest, size


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    state = collections.OrderedDict()

    # Kept, 4-D, fp16.
    state["feature_model.decoder.convs.0.weight"] = probe((8, 16, 1, 1), 0)
    # Kept, 4-D, fp16, a long name like the real encoder blocks (84 characters).
    state["monodepth_model.monodepth_predictor.encoder.patch_encoder.blocks.23.attn.proj.weight"] = probe((8, 16, 1, 1), 3)
    # Kept, 2-D, fp16 (a ViT MLP weight, the largest live tensor in the model).
    state["monodepth_model.monodepth_predictor.encoder.patch_encoder.blocks.0.mlp.fc1.weight"] = probe((16, 8), 5)
    # Kept, 3-D, fp16.
    state["monodepth_model.monodepth_predictor.decoder.convs.0.weight"] = probe((2, 3, 4), 7)
    # Kept, 1-D, stays fp32 (LayerNorm weight/bias).
    state["monodepth_model.monodepth_predictor.decoder.norm.weight"] = probe((16,), 1)
    state["monodepth_model.monodepth_predictor.decoder.norm.bias"] = probe((16,), 2)
    # Kept, 1-D, stays fp32 (a conv bias).
    state["feature_model.decoder.convs.1.bias"] = probe((7,), 4)
    # Kept, under a shallow prefix that is not stripped.
    state["prediction_head.weight"] = probe((2, 4, 1, 1), 9)
    state["prediction_head.bias"] = probe((2,), 6)

    # Dropped: the timm ImageNet classifier head, which the reference never calls.
    state["monodepth_model.monodepth_predictor.encoder.patch_encoder.head.weight"] = probe((32, 16), 11)
    state["monodepth_model.monodepth_predictor.encoder.patch_encoder.head.bias"] = probe((32,), 12)
    # Dropped: the depth-adjustment U-Net, dead at inference.
    state["depth_alignment.scale_map_estimator.conv_out.weight"] = probe((1, 16, 1, 1), 13)
    state["depth_alignment.scale_map_estimator.conv_out.bias"] = probe((1,), 14)

    dropped = [name for name in state if name.startswith("depth_alignment.")
               or name.endswith("patch_encoder.head.weight")
               or name.endswith("patch_encoder.head.bias")]

    torch.save(state, OUT_PT)
    reorder_storage_entries(OUT_PT)

    size = os.path.getsize(OUT_PT)
    digest = hashlib.sha256(open(OUT_PT, "rb").read()).hexdigest()
    document = {
        "source": "synthetic.pt",
        "checkpoint_sha256": digest,
        "checkpoint_bytes": size,
        "tensor_count": len(state),
        "converted_tensor_count": len(state) - len(dropped),
        "dropped_count": len(dropped),
        "dropped": dropped,
        "tensor_order": list(state.keys()),
        "tensors": manifest(state, set(dropped)),
    }
    with open(OUT_JSON, "w") as handle:
        json.dump(document, handle, indent=1)
        handle.write("\n")

    total = size + os.path.getsize(OUT_JSON)
    print(f"wrote {OUT_PT} ({size} B) and {OUT_JSON} ({os.path.getsize(OUT_JSON)} B)")
    print(f"{len(state)} tensors, {len(state) - len(dropped)} kept, {len(dropped)} dropped, "
          f"{total} B of fixture")
    assert total < 200 * 1024, "fixture budget is 200 KB"


if __name__ == "__main__":
    if sys.argv[1:] == ["--reorder-only"]:
        reorder_storage_entries(OUT_PT)
        digest, size = update_manifest_digest()
        print(f"reordered {OUT_PT} ({size} B), sha256 {digest}")
    else:
        import torch  # imported here so --reorder-only runs without the venv

        main()
