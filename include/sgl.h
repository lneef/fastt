#pragma once

#include "slab_allocator.h"
#include <cstdint>
struct sgl{
    mbuf_ptr head;
    mbuf* tail;
    size_t size;
    uint32_t segs;

    sgl(): head(nullptr, mbuf_free), tail(nullptr), size(0), segs(0){}

    sgl(sgl &&other) noexcept : head(std::move(other.head)), tail(other.tail), size(other.size), segs(other.segs){
        other.size = 0;
        other.segs = 0;
    }

    sgl& operator=(sgl&& other){
        new (this) sgl(std::move(other));
        return *this;
    }

    struct iterator {
        mbuf *cur;
        iterator(mbuf *m) : cur(m) {}
        mbuf &operator*() const { return *cur; }
        mbuf *operator->() const { return cur; }
        iterator &operator++() { cur = cur->next; return *this; }
        bool operator!=(const iterator &o) const { return cur != o.cur; }
        bool operator==(const iterator &o) const { return cur == o.cur; }
    };

    iterator begin() { return {head.get()}; }
    iterator end() { return {nullptr}; }

    void add_segment_safe(mbuf_ptr &&ptr){
        if(!head){
            head = std::move(ptr);
            tail = head->last_seg(size, segs);
        }else{
            auto *last = ptr->last_seg(size, segs);
            tail->next = ptr.release();
            tail = last;
            assert(tail != nullptr);
        }
    }

    mbuf_ptr take_head() && {
        auto head_next = head->next;
        auto head_ptr = std::move(head);
        head_ptr->next = nullptr;
        head = mbuf_take_owner_ship(head_next);
        size -= head_ptr->data_len;
        --segs;
        return head_ptr;
    }

    bool empty() const{
        return head == nullptr;
    }

    void combine(sgl& other){
        tail->next = other.head.release();
        tail = other.tail;
        size += other.size;
        segs += other.segs;
    }

    mbuf* alloc_message(slab_allocator &slab, size_t len){
        mbuf *first = nullptr;
        while(len > 0){
            auto chunk = static_cast<uint16_t>(std::min(len, (size_t)slab_allocator::kMaxDataLen));
            auto seg = slab.alloc_default_safe(chunk);
            add_segment_safe(std::move(seg));
            if(!first)
                first = tail;
            len -= chunk;
        }
        return first;
    }

};
