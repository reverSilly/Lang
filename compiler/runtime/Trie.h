#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

namespace reverSilly {

template <typename V>
class Trie {
public:
    struct Node {
        std::unordered_map<uint8_t, std::unique_ptr<Node>> sons;
        std::optional<V> value;
    };

private:
    Node root_;

public:
    Trie() = default;

    V* find(std::span<const uint8_t> key) {
        Node* cur = &root_;
        for (uint8_t b : key) {
            auto it = cur->sons.find(b);
            if (it == cur->sons.end()) {
                return nullptr;
            }
            cur = it->second.get();
        }
        return cur->value ? &(*cur->value) : nullptr;
    }

    const V* find(std::span<const uint8_t> key) const {
        const Node* cur = &root_;
        for (uint8_t b : key) {
            auto it = cur->sons.find(b);
            if (it == cur->sons.end()) {
                return nullptr;
            }
            cur = it->second.get();
        }
        return cur->value ? &(*cur->value) : nullptr;
    }

    V& insert_or_assign(std::span<const uint8_t> key, V val) {
        Node* cur = &root_;
        for (uint8_t b : key) {
            auto& child = cur->sons[b];
            if (!child) {
                child = std::make_unique<Node>();
            }
            cur = child.get();
        }
        cur->value = std::move(val);
        return *cur->value;
    }

    template <typename Factory>
    V& get_or_emplace(std::span<const uint8_t> key, Factory&& factory) {
        Node* cur = &root_;
        for (uint8_t b : key) {
            auto& child = cur->sons[b];
            if (!child) {
                child = std::make_unique<Node>();
            }
            cur = child.get();
        }
        if (!cur->value) {
            cur->value = factory();
        }
        return *cur->value;
    }
};

} // namespace reverSilly
