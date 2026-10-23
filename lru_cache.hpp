#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace sentrix
{

    class LRUCache
    {
    private:
        struct Node
        {
            std::string key;
            std::string value;
            Node *prev = nullptr;
            Node *next = nullptr;

            Node(std::string k, std::string v)
                : key(std::move(k)), value(std::move(v)) {}
        };

    public:
        explicit LRUCache(std::size_t capacity) : capacity_(capacity)
        {
            head_.next = &tail_;
            tail_.prev = &head_;
            index_.reserve(capacity);
        }

        LRUCache(const LRUCache &) = delete;
        LRUCache &operator=(const LRUCache &) = delete;
        LRUCache(LRUCache &&) = delete;
        LRUCache &operator=(LRUCache &&) = delete;

        std::optional<std::string> get(const std::string &key)
        {
            auto it = index_.find(key);
            if (it == index_.end())
            {
                ++misses_;
                return std::nullopt;
            }
            ++hits_;
            Node *node = it->second.get();
            move_to_front(node);
            return node->value;
        }

        void put(std::string key, std::string value)
        {
            if (capacity_ == 0)
            {
                return;
            }

            auto it = index_.find(key);
            if (it != index_.end())
            {
                Node *node = it->second.get();
                payload_bytes_ -= node->value.capacity();
                node->value = std::move(value);
                payload_bytes_ += node->value.capacity();
                move_to_front(node);
                return;
            }

            if (index_.size() >= capacity_)
            {
                evict_least_recently_used();
            }

            auto node = std::make_unique<Node>(std::move(key), std::move(value));
            Node *raw = node.get();
            payload_bytes_ += raw->key.capacity() + raw->value.capacity();
            link_after_head(raw);
            index_.emplace(raw->key, std::move(node));
        }

        bool contains(const std::string &key) const
        {
            return index_.find(key) != index_.end();
        }

        void record_fast_miss() noexcept
        {
            ++misses_;
        }

        std::size_t size() const noexcept { return index_.size(); }
        std::size_t capacity() const noexcept { return capacity_; }
        std::size_t hits() const noexcept { return hits_; }
        std::size_t misses() const noexcept { return misses_; }

        std::size_t memory_usage_bytes() const noexcept
        {
            const std::size_t node_overhead = index_.size() * (sizeof(Node) + sizeof(void *) * 3);
            const std::size_t bucket_overhead = index_.bucket_count() * sizeof(void *);
            return sizeof(*this) + node_overhead + bucket_overhead + payload_bytes_;
        }

    private:
        void unlink(Node *node) noexcept
        {
            node->prev->next = node->next;
            node->next->prev = node->prev;
        }

        void link_after_head(Node *node) noexcept
        {
            node->prev = &head_;
            node->next = head_.next;
            head_.next->prev = node;
            head_.next = node;
        }

        void move_to_front(Node *node) noexcept
        {
            unlink(node);
            link_after_head(node);
        }

        void evict_least_recently_used()
        {
            Node *victim = tail_.prev;
            if (victim == &head_)
            {
                return;
            }
            unlink(victim);
            payload_bytes_ -= (victim->key.capacity() + victim->value.capacity());
            index_.erase(victim->key);
        }

        std::size_t capacity_;
        Node head_{"", ""};
        Node tail_{"", ""};
        std::unordered_map<std::string, std::unique_ptr<Node>> index_;
        std::size_t hits_ = 0;
        std::size_t misses_ = 0;
        std::size_t payload_bytes_ = 0;
    };

} // namespace sentrix