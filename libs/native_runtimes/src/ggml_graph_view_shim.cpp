#include "../../third_party/llama.cpp/ggml/src/ggml-impl.h"

struct ggml_cgraph ggml_graph_view(struct ggml_cgraph* cgraph0, int i0, int i1) {
    struct ggml_cgraph cgraph = {
        0, i1 - i0, 0, cgraph0->nodes + i0, nullptr, nullptr, nullptr,
        cgraph0->use_counts, cgraph0->visited_hash_set, cgraph0->order, 0};
    return cgraph;
}
