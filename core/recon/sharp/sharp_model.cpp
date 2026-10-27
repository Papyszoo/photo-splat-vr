#include "recon/sharp/sharp_model.h"

#include <utility>

namespace splat {

Status SharpModel::initialize(const Path& gguf, bool prefer_vulkan) {
    std::string note;
    Result<GgmlBackend> backend = GgmlBackend::create(prefer_vulkan, &note);
    if (!backend.ok()) return Status::error(backend.message());
    backend_ = std::move(backend.value());

    Result<GgmlWeights> weights = GgmlWeights::load(gguf, backend_);
    if (!weights.ok()) return Status::error(weights.message());
    weights_ = std::move(weights.value());

    const std::string architecture = weights_.architecture();
    if (architecture != "sharp") {
        return Status::error(gguf.string() + ": architecture '" + architecture +
                             "', expected 'sharp' (convert with --backend sharp)");
    }
    path_ = gguf;
    return Status::success();
}

Result<std::shared_ptr<SharpModel>> SharpModel::load(const Path& gguf, bool prefer_vulkan) {
    using ResultT = Result<std::shared_ptr<SharpModel>>;
    auto model = std::shared_ptr<SharpModel>(new SharpModel());
    const Status status = model->initialize(gguf, prefer_vulkan);
    if (!status.ok()) return ResultT::failure(status.message());
    return ResultT::success(std::move(model));
}

}  // namespace splat
