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
            segs = ptr->nb_segs;
            size = ptr->data_len;
            head = std::move(ptr);
            tail = head->last_seg();
        }else{
            segs += ptr->nb_segs;
            size += ptr->data_len;
            auto *last = ptr->last_seg();
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

};
