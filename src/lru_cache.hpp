#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace sentrix {

// Forward unique_ptr links own nodes in the list; prev and index_ hold non-owning pointers.
class LRUCache {
private:
    struct Node {
        std::string key;
        std::string value;
        Node* prev = nullptr;
        std::unique_ptr<Node> next;

        Node(std::string k, std::string v) : key(std::move(k)), value(std::move(v)) {}
    };

public:
    explicit LRUCache(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ > 0) {
            index_.reserve(std::min<std::size_t>(capacity_, 1'000'000));
        }
    }

    ~LRUCache() { clear(); }

    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;
    LRUCache(LRUCache&&) = delete;
    LRUCache& operator=(LRUCache&&) = delete;

    std::optional<std::string> get(std::string_view key) {
        auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return std::nullopt;
        }
        ++hits_;
        move_to_front(it->second);
        return it->second->value;
    }

    const std::string* peek(std::string_view key) const noexcept {
        auto it = index_.find(key);
        return (it == index_.end()) ? nullptr : &it->second->value;
    }

    void put(std::string key, std::string value) {
        if (capacity_ == 0) return;
        if (auto it = index_.find(key); it != index_.end()) {
            it->second->value = std::move(value);
            move_to_front(it->second);
            return;
        }
        insert_absent(std::move(key), std::move(value));
    }

    void put_known_absent(std::string key, std::string value) {
        if (capacity_ == 0) return;
        insert_absent(std::move(key), std::move(value));
    }

    bool contains(std::string_view key) const noexcept {
        return index_.find(key) != index_.end();
    }

    // Iterative unlink avoids recursive ~unique_ptr stack overflow on large caches.
    void clear() noexcept {
        index_.clear();
        while (head_) {
            std::unique_ptr<Node> next = std::move(head_->next);
            head_ = std::move(next);
        }
        tail_ = nullptr;
    }

    std::size_t size() const noexcept { return index_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t hits() const noexcept { return hits_; }
    std::size_t misses() const noexcept { return misses_; }
    std::size_t evictions() const noexcept { return evictions_; }

    std::size_t approx_memory_bytes() const noexcept {
        std::size_t bytes = index_.bucket_count() * sizeof(void*) +
                            index_.size() * (sizeof(std::pair<const std::string_view, Node*>) + 2 * sizeof(void*));
        for (const Node* cur = head_.get(); cur; cur = cur->next.get()) {
            bytes += sizeof(Node);
            if (cur->key.capacity() > 15) bytes += cur->key.capacity() + 1;
            if (cur->value.capacity() > 15) bytes += cur->value.capacity() + 1;
        }
        return bytes;
    }

private:
    void push_front(std::unique_ptr<Node> node) noexcept {
        node->prev = nullptr;
        node->next = std::move(head_);
        if (node->next) {
            node->next->prev = node.get();
        } else {
            tail_ = node.get();
        }
        head_ = std::move(node);
    }

    std::unique_ptr<Node> detach(Node* node) noexcept {
        std::unique_ptr<Node> owned;
        if (node->prev) {
            owned = std::move(node->prev->next);
            node->prev->next = std::move(node->next);
            if (node->prev->next) {
                node->prev->next->prev = node->prev;
            } else {
                tail_ = node->prev;
            }
        } else {
            owned = std::move(head_);
            head_ = std::move(node->next);
            if (head_) {
                head_->prev = nullptr;
            } else {
                tail_ = nullptr;
            }
        }
        node->prev = nullptr;
        return owned;
    }

    void move_to_front(Node* node) noexcept {
        if (head_.get() != node) {
            push_front(detach(node));
        }
    }

    void insert_absent(std::string key, std::string value) {
        std::unique_ptr<Node> node;
        if (index_.size() >= capacity_ && tail_) {
            index_.erase(tail_->key);  // Must erase string_view key before overwriting tail_->key.
            node = detach(tail_);
            ++evictions_;
            node->key = std::move(key);
            node->value = std::move(value);
        } else {
            node = std::make_unique<Node>(std::move(key), std::move(value));
        }
        Node* raw = node.get();
        index_.emplace(std::string_view{raw->key}, raw);
        push_front(std::move(node));
    }

    std::size_t capacity_;
    std::unique_ptr<Node> head_;
    Node* tail_ = nullptr;
    std::unordered_map<std::string_view, Node*> index_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
    std::size_t evictions_ = 0;
};

} // namespace sentrix
