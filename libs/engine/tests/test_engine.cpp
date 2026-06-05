#include <catch2/catch_test_macros.hpp>

#include "engine/slot.hpp"
#include "engine/slot_pool.hpp"
#include "engine/token_sequence.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using inferdeck::engine::AcquireOptions;
using inferdeck::engine::AcquireStatus;
using inferdeck::engine::Slot;
using inferdeck::engine::SlotMatch;
using inferdeck::engine::SlotPool;
using inferdeck::engine::SlotState;
using inferdeck::engine::TokenSequence;

TEST_CASE("TokenSequence: lcp basic", "[engine][tokens]") {
    TokenSequence a({1, 2, 3, 4, 5});
    TokenSequence b({1, 2, 3, 9, 8});
    REQUIRE(a.lcp_with(b) == 3);
}

TEST_CASE("TokenSequence: lcp empty", "[engine][tokens]") {
    TokenSequence a;
    TokenSequence b({1, 2, 3});
    REQUIRE(a.lcp_with(b) == 0);
    TokenSequence c({1, 2});
    TokenSequence d({3, 4});
    REQUIRE(c.lcp_with(d) == 0);
}

TEST_CASE("TokenSequence: lcp full match", "[engine][tokens]") {
    TokenSequence a({1, 2, 3});
    TokenSequence b({1, 2, 3});
    REQUIRE(a.lcp_with(b) == 3);
}

TEST_CASE("TokenSequence: lcp prefix of other", "[engine][tokens]") {
    TokenSequence a({1, 2, 3});
    TokenSequence b({1, 2, 3, 4, 5});
    REQUIRE(a.lcp_with(b) == 3);
    REQUIRE(b.lcp_with(a) == 3);
}

TEST_CASE("Slot: idle by default, mark busy/idle", "[engine][slot]") {
    Slot s(0);
    REQUIRE(s.id() == 0);
    REQUIRE_FALSE(s.is_busy());
    s.mark_busy();
    REQUIRE(s.is_busy());
    s.mark_idle();
    REQUIRE_FALSE(s.is_busy());
}

TEST_CASE("Slot: prev_tokens round-trip", "[engine][slot]") {
    Slot s(0);
    s.set_prev_tokens(TokenSequence({1, 2, 3}));
    auto prev = s.prev_tokens();
    REQUIRE(prev.size() == 3);
    REQUIRE(prev.at(0) == 1);
    REQUIRE(prev.at(1) == 2);
    REQUIRE(prev.at(2) == 3);
    s.append_to_prev({4, 5});
    prev = s.prev_tokens();
    REQUIRE(prev.size() == 5);
    s.trim_prev_to(2);
    prev = s.prev_tokens();
    REQUIRE(prev.size() == 2);
    REQUIRE(prev.at(0) == 1);
    REQUIRE(prev.at(1) == 2);
}

TEST_CASE("SlotPool: n_slots/n_free/n_busy", "[engine][pool]") {
    SlotPool p(2);
    REQUIRE(p.n_slots() == 2);
    REQUIRE(p.n_free() == 2);
    REQUIRE(p.n_busy() == 0);
    (void)p.acquire_with_match(TokenSequence({1, 2, 3}));
    REQUIRE(p.n_free() == 1);
    REQUIRE(p.n_busy() == 1);
}

TEST_CASE("SlotPool: acquire returns free slot with no LCP", "[engine][pool]") {
    SlotPool p(2);
    TokenSequence incoming({1, 2, 3});
    auto m = p.acquire_with_match(incoming);
    REQUIRE(m.slot_id == 0);
    REQUIRE(m.prefix_tokens == 0);
    REQUIRE_FALSE(m.lcp_hit);
    REQUIRE(p.slot(0).is_busy());
}

TEST_CASE("SlotPool: LCP-match prefers longest prefix", "[engine][pool]") {
    SlotPool p(2);
    p.slot(0).set_prev_tokens(TokenSequence({1, 2, 3, 4, 5}));
    p.slot(1).set_prev_tokens(TokenSequence({1, 2, 9, 10}));

    TokenSequence incoming({1, 2, 3, 4, 9});
    auto best = p.best_lcp_match(incoming);
    REQUIRE(best.has_value());
    REQUIRE(best->slot_id == 0);
    REQUIRE(best->prefix_tokens == 4);
    REQUIRE(best->lcp_hit);
}

TEST_CASE("SlotPool: busy slots are skipped in LCP-match", "[engine][pool]") {
    SlotPool p(2);
    p.slot(0).set_prev_tokens(TokenSequence({1, 2, 3, 4, 5}));
    p.slot(0).mark_busy();
    p.slot(1).set_prev_tokens(TokenSequence({1, 2, 9, 10}));

    TokenSequence incoming({1, 2, 3, 4, 9});
    auto m = p.acquire_with_match(incoming);
    REQUIRE(m.slot_id == 1);
    REQUIRE(m.prefix_tokens == 2);
    REQUIRE(m.lcp_hit);
}

TEST_CASE("SlotPool: LCP length 0 slots are skipped", "[engine][pool]") {
    SlotPool p(2);
    p.slot(0).set_prev_tokens(TokenSequence({1, 2, 3}));
    TokenSequence incoming({9, 8, 7});
    auto best = p.best_lcp_match(incoming);
    REQUIRE_FALSE(best.has_value());
    auto m = p.acquire_with_match(incoming);
    REQUIRE(m.slot_id == 0);
    REQUIRE(m.prefix_tokens == 0);
    REQUIRE_FALSE(m.lcp_hit);
}

TEST_CASE("SlotPool: all busy non-blocking returns timeout", "[engine][pool]") {
    SlotPool p(2);
    (void)p.acquire_with_match(TokenSequence({1, 2, 3}));
    (void)p.acquire_with_match(TokenSequence({4, 5, 6}));
    REQUIRE(p.n_free() == 0);
    AcquireOptions opts;
    opts.block = false;
    auto [status, m] = p.acquire(TokenSequence({7, 8, 9}), opts);
    REQUIRE(status == AcquireStatus::Timeout);
    REQUIRE(m.slot_id == -1);
}

TEST_CASE("SlotPool: blocking acquire times out when all busy", "[engine][pool]") {
    SlotPool p(2);
    (void)p.acquire_with_match(TokenSequence({1, 2, 3}));
    (void)p.acquire_with_match(TokenSequence({4, 5, 6}));
    AcquireOptions opts;
    opts.timeout = std::chrono::milliseconds{100};
    auto start = std::chrono::steady_clock::now();
    auto [status, m] = p.acquire(TokenSequence({7, 8, 9}), opts);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    REQUIRE(status == AcquireStatus::Timeout);
    REQUIRE(elapsed >= 90);
    REQUIRE(elapsed < 1000);
}

TEST_CASE("SlotPool: blocking acquire succeeds after release", "[engine][pool]") {
    SlotPool p(2);
    auto m1 = p.acquire_with_match(TokenSequence({1, 2, 3}));
    auto m2 = p.acquire_with_match(TokenSequence({4, 5, 6}));

    std::thread releaser([&p, m1]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        p.release(m1.slot_id);
    });

    AcquireOptions opts;
    opts.timeout = std::chrono::milliseconds{2000};
    auto [status, m3] = p.acquire(TokenSequence({7, 8, 9}), opts);
    releaser.join();
    REQUIRE(status == AcquireStatus::Acquired);
    REQUIRE(m3.slot_id == m1.slot_id);
    REQUIRE(p.n_free() == 0);
}

TEST_CASE("SlotPool: parallel acquire/release does not deadlock", "[engine][pool]") {
    SlotPool p(2);
    std::atomic<int> success{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&p, &success, i]() {
            TokenSequence ts({i, i + 100, i + 200});
            AcquireOptions opts;
            opts.timeout = std::chrono::milliseconds{2000};
            auto [status, m] = p.acquire(ts, opts);
            if (status == AcquireStatus::Acquired) {
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
                p.release(m.slot_id);
                ++success;
            }
        });
    }
    for (auto& t : threads) t.join();
    REQUIRE(success.load() == 8);
    REQUIRE(p.n_free() == 2);
}

TEST_CASE("Engine: 2 concurrent opencode requests, shared prefix hits slot 0", "[engine][concurrent]") {
    SlotPool p(2);

    TokenSequence shared_prefix({100, 101, 102, 103});
    TokenSequence req_a(shared_prefix.tokens());
    req_a.append(200);
    req_a.append(201);

    TokenSequence req_b(shared_prefix.tokens());
    req_b.append(300);
    req_b.append(301);

    auto m_a = p.acquire_with_match(req_a);
    REQUIRE(m_a.slot_id == 0);
    REQUIRE(m_a.prefix_tokens == 0);
    p.slot(m_a.slot_id).set_prev_tokens(req_a);

    auto m_b = p.acquire_with_match(req_b);
    REQUIRE(m_b.slot_id == 1);
    REQUIRE(m_b.prefix_tokens == 0);

    p.slot(0).mark_idle();
    TokenSequence req_c(shared_prefix.tokens());
    req_c.append(400);
    auto m_c = p.acquire_with_match(req_c);
    REQUIRE(m_c.slot_id == 0);
    REQUIRE(m_c.prefix_tokens == 4);
    REQUIRE(m_c.lcp_hit);
}

TEST_CASE("Engine: 2 divergent prompts land in separate slots", "[engine][concurrent]") {
    SlotPool p(2);

    TokenSequence req_a({1, 2, 3, 4, 5});
    TokenSequence req_b({10, 20, 30, 40, 50});

    auto m_a = p.acquire_with_match(req_a);
    p.slot(m_a.slot_id).set_prev_tokens(req_a);

    auto m_b = p.acquire_with_match(req_b);
    p.slot(m_b.slot_id).set_prev_tokens(req_b);

    REQUIRE(m_a.slot_id != m_b.slot_id);
    REQUIRE(p.slot(0).sequence_length() == 5);
    REQUIRE(p.slot(1).sequence_length() == 5);
}

TEST_CASE("Engine: LCP trim preserves prefix tokens", "[engine][concurrent]") {
    SlotPool p(2);
    TokenSequence initial({1, 2, 3, 4, 5, 6, 7, 8});
    p.slot(0).set_prev_tokens(initial);

    TokenSequence shorter({1, 2, 3, 4});
    auto m = p.acquire_with_match(shorter);
    REQUIRE(m.lcp_hit);
    REQUIRE(m.prefix_tokens == 4);

    p.slot(0).trim_prev_to(m.prefix_tokens);
    REQUIRE(p.slot(0).sequence_length() == 4);
}
