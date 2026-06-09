#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "model/backend_coordinator.hpp"
#include "model/imodel.hpp"
#include "model/model_registry.hpp"

#include "report_printer.hpp"
#include "swap_exerciser.hpp"
#include "swap_report.hpp"

using inferdeck::model::BackendCoordinator;
using inferdeck::model::IModel;
using inferdeck::model::InferenceRequest;
using inferdeck::model::ModelInfo;
using inferdeck::model::ModelRegistry;
using inferdeck::apps::SwapReport;
using inferdeck::apps::do_swap;
using inferdeck::apps::print_report;

namespace {

class DummyModel : public IModel {
public:
    ModelInfo info_{};
    bool loaded_{false};
    int vram_mb_{4096};
    int n_slots_{2};
    std::vector<int> busy_;
    int predict_count_{0};
    int load_delay_ms_{0};
    int unload_delay_ms_{0};
    inferdeck::model::ChatTemplateMeta chat_meta_{};

    explicit DummyModel(ModelInfo i) : info_(std::move(i)) {
        busy_.assign(n_slots_, 0);
    }

    const ModelInfo& info() const override { return info_; }
    const inferdeck::model::ChatTemplateMeta& chat_template_meta() const override { return chat_meta_; }
    inferdeck::foundation::Result<void> load() override {
        if (load_delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(load_delay_ms_));
        }
        loaded_ = true;
        return inferdeck::foundation::Ok();
    }
    inferdeck::foundation::Result<void> unload() override {
        if (unload_delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(unload_delay_ms_));
        }
        loaded_ = false;
        std::fill(busy_.begin(), busy_.end(), 0);
        return inferdeck::foundation::Ok();
    }
    bool is_loaded() const override { return loaded_; }
    int vram_usage_mb() const override { return vram_mb_; }
    int n_slots() const override { return n_slots_; }
    int n_free_slots() const override {
        return n_slots_ - static_cast<int>(std::count(busy_.begin(), busy_.end(), 1));
    }
    inferdeck::foundation::Result<int> acquire_slot() override {
        for (int i = 0; i < static_cast<int>(busy_.size()); ++i) {
            if (busy_[i] == 0) { busy_[i] = 1; return inferdeck::foundation::Ok(i); }
        }
        return inferdeck::foundation::Err<int>(inferdeck::foundation::ErrorCode::Unavailable, "no slots");
    }
    inferdeck::foundation::Result<void> release_slot(int slot_id) override {
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_.size())) {
            return inferdeck::foundation::Err<void>(inferdeck::foundation::ErrorCode::InvalidArgument, "");
        }
        busy_[slot_id] = 0;
        return inferdeck::foundation::Ok();
    }
    bool slot_busy(int slot_id) const override {
        if (slot_id < 0 || slot_id >= static_cast<int>(busy_.size())) return false;
        return busy_[slot_id] != 0;
    }
    inferdeck::foundation::Result<inferdeck::model::InferenceResult> predict(
        int, const InferenceRequest&) override {
        ++predict_count_;
        inferdeck::model::InferenceResult r;
        r.text = "ok";
        r.prompt_tokens = 8;
        r.completion_tokens = 4;
        r.duration_ms = 50.0f;
        r.tokens_per_second = 80.0f;
        return inferdeck::foundation::Ok(r);
    }
};

ModelInfo make_info(const std::string& name, int n_slots, int vram, bool vision) {
    ModelInfo i;
    i.name = name;
    i.family = (name.find("coder") != std::string::npos) ? "qwen3-coder" : "qwen3.6";
    i.gguf_path = "C:/models/" + name + ".gguf";
    i.n_slots = n_slots;
    i.vram_required_mb = vram;
    i.context_size = 65536;
    i.has_vision = vision;
    return i;
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "[model-tester] InferDeck model-tester starting" << std::endl;

    ModelRegistry reg;
    reg.set_factory([](const ModelInfo& i) -> std::unique_ptr<IModel> {
        return std::make_unique<DummyModel>(i);
    });
    reg.register_model(make_info("qwen3.6-27b", 2, 22000, true));
    reg.register_model(make_info("qwen3-coder-next", 2, 24000, false));

    BackendCoordinator coord(reg);

    std::vector<SwapReport> reports;
    reports.push_back(do_swap(coord, "", "qwen3.6-27b"));
    print_report(reports.back());
    reports.push_back(do_swap(coord, "qwen3.6-27b", "qwen3-coder-next"));
    print_report(reports.back());
    reports.push_back(do_swap(coord, "qwen3-coder-next", "qwen3.6-27b"));
    print_report(reports.back());

    std::cout << "[model-tester] exercise: 2 concurrent slots, predict, release" << std::endl;
    auto s1 = coord.acquire_slot("qwen3.6-27b");
    auto s2 = coord.acquire_slot("qwen3.6-27b");
    if (s1.has_value() && s2.has_value()) {
        InferenceRequest req;
        req.prompt = "hello";
        req.max_tokens = 16;
        auto r1 = coord.predict("qwen3.6-27b", s1.value(), req);
        auto r2 = coord.predict("qwen3.6-27b", s2.value(), req);
        std::cout << "[model-tester] predict 1 ok=" << r1.has_value()
                  << " predict 2 ok=" << r2.has_value() << std::endl;
        (void)coord.release_slot("qwen3.6-27b", s1.value());
        (void)coord.release_slot("qwen3.6-27b", s2.value());
    }

    int passed = 0;
    int failed = 0;
    int64_t total_ms = 0;
    for (const auto& rep : reports) {
        if (rep.success) { ++passed; total_ms += rep.total_ms.count(); }
        else ++failed;
    }
    std::cout << "[model-tester] summary: " << passed << " passed, "
              << failed << " failed, total=" << total_ms << "ms" << std::endl;

    if (failed > 0) return 1;
    if (total_ms > 30000) {
        std::cout << "[model-tester] WARNING: total swap time exceeded 30s" << std::endl;
        return 2;
    }
    std::cout << "[model-tester] DONE" << std::endl;
    return 0;
}
