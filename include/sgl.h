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

    void write(const void *buf, size_t buf_len, size_t off = 0){
        auto *src = static_cast<const uint8_t *>(buf);
        auto *seg = head.get();
        while(off >= seg->data_len){
            off -= seg->data_len;
            seg = seg->next;
        }
        size_t buf_off = 0;
        while(seg && buf_off < buf_len){
            auto len = seg->data_len - off;
            len = std::min(len, buf_len - buf_off);
            std::memcpy(seg->data<uint8_t>() + off, src + buf_off, len);
            buf_off += len;
            off = 0;
            seg = seg->next;
        }
    }

    void alloc_message(slab_allocator &slab, size_t len){
        while(len > 0){
            auto chunk = static_cast<uint16_t>(std::min(len, (size_t)slab_allocator::kMaxDataLen));
            add_segment_safe(slab.alloc_default_safe(chunk));
            len -= chunk;
        }
    }

};
