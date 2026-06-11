#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace inferdeck::foundation {

struct BusEvent {
    std::string name;
    std::string data;
};

class EventBus {
public:
    class Subscription {
    public:
        explicit Subscription(std::size_t max_queue) : max_queue_(max_queue) {}

        std::optional<BusEvent> wait_for(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait_for(lk, timeout, [&] { return !queue_.empty() || closed_; });
            if (queue_.empty()) return std::nullopt;
            BusEvent ev = std::move(queue_.front());
            queue_.pop_front();
            return ev;
        }

        void push(const BusEvent& ev) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (closed_) return;
                if (queue_.size() >= max_queue_) queue_.pop_front();
                queue_.push_back(ev);
            }
            cv_.notify_one();
        }

        void close() {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                closed_ = true;
            }
            cv_.notify_all();
        }

        [[nodiscard]] bool closed() const {
            std::lock_guard<std::mutex> lk(mtx_);
            return closed_;
        }

    private:
        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::deque<BusEvent> queue_;
        std::size_t max_queue_;
        bool closed_{false};
    };

    std::shared_ptr<Subscription> subscribe(std::size_t max_queue = 256) {
        auto sub = std::make_shared<Subscription>(max_queue);
        std::lock_guard<std::mutex> lk(mtx_);
        subs_.push_back(sub);
        return sub;
    }

    void publish(const std::string& name, const std::string& data) {
        std::vector<std::shared_ptr<Subscription>> targets;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            std::erase_if(subs_, [](const std::weak_ptr<Subscription>& w) { return w.expired(); });
            targets.reserve(subs_.size());
            for (const auto& w : subs_) {
                if (auto s = w.lock()) targets.push_back(std::move(s));
            }
        }
        for (auto& s : targets) s->push({name, data});
    }

    [[nodiscard]] std::size_t subscriber_count() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::erase_if(subs_, [](const std::weak_ptr<Subscription>& w) { return w.expired(); });
        return subs_.size();
    }

    void close_all() {
        std::vector<std::weak_ptr<Subscription>> subs;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            subs.swap(subs_);
        }
        for (const auto& w : subs) {
            if (auto s = w.lock()) s->close();
        }
    }

private:
    std::mutex mtx_;
    std::vector<std::weak_ptr<Subscription>> subs_;
};

} // namespace inferdeck::foundation
