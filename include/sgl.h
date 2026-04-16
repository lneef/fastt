#pragma once

#include "slab_allocator.h"
#include <cstdint>
#include <rte_memcpy.h>
struct sgl {
  mbuf_ptr head;
  mbuf *tail;
  size_t size;
  uint32_t segs;

  sgl() : head(nullptr, mbuf_free), tail(nullptr), size(0), segs(0) {}

  sgl(sgl &&other) noexcept
      : head(std::move(other.head)), tail(other.tail), size(other.size),
        segs(other.segs) {
    other.size = 0;
    other.segs = 0;
  }

  sgl &operator=(sgl &&other) {
    new (this) sgl(std::move(other));
    return *this;
  }

  void coalesce() {
    if (!head)
      return;
    auto *m = head.get();
    auto *next = head->next;
    auto droom = m->data_room;
    for (; next;) {
      if (next->data_len + m->data_len >= droom) {
        m->next = next;
        m = next;
        droom = m->data_room;
        next = m->next;
      } else {
        rte_memcpy(m->data<void>(m->data_len), next->data<void>(),
                   next->data_len);
        m->data_len += next->data_len;
        auto *to_del = next;
        next = next->next;
        m->next = next;
        mbuf_free(to_del);
        --segs;
      }
    }
    tail = m;
  }

  struct iterator {
    mbuf *cur;
    iterator(mbuf *m) : cur(m) {}
    mbuf &operator*() const { return *cur; }
    mbuf *operator->() const { return cur; }
    iterator &operator++() {
      cur = cur->next;
      return *this;
    }
    bool operator!=(const iterator &o) const { return cur != o.cur; }
    bool operator==(const iterator &o) const { return cur == o.cur; }
  };

  iterator begin() { return {head.get()}; }
  iterator end() { return {nullptr}; }

  void add_segment_safe(mbuf_ptr &&ptr) {
    if (!head) {
      head = std::move(ptr);
      tail = head->last_seg(size, segs);
    } else {
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

  bool empty() const { return head == nullptr; }

  void combine(sgl &other) {
    tail->next = other.head.release();
    tail = other.tail;
    size += other.size;
    segs += other.segs;
  }

  void alloc_message(slab_allocator &slab, size_t len) {
    while (len > 0) {
      size_t chunk = 0;
      mbuf *seg;
      if (len / slab_allocator::kMaxDataLen >= 8) {
        seg = slab.alloc_large();
        chunk = slab_allocator::kMaxJumboDataLen;
      } else {
        chunk = static_cast<uint16_t>(
            std::min(len, slab_allocator::kMaxDataLen));
        seg = slab.alloc_default(chunk);
      }
      add_segment_safe(mbuf_take_owner_ship(seg));
      len -= chunk;
    }
  }
};
