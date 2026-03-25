#pragma once
#include "transport/protocol.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <generic/rte_prefetch.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
class slab_allocator;

struct mbuf {
  slab_allocator *sb;
  mbuf *next;
  uint32_t size;
  uint16_t data_len;
  uint16_t headroom : 15;
  uint16_t xmit : 1;

  mbuf() = default;
  mbuf(slab_allocator *sb, mbuf *next, uint32_t size, uint16_t data_len,
       uint16_t headroom, bool xmit)
      : sb(sb), next(next), size(size), data_len(data_len), headroom(headroom),
        xmit(xmit) {}

  uint8_t *buf_start() {
    return reinterpret_cast<uint8_t *>(this) + sizeof(mbuf);
  }

  template <typename T> T *data(size_t offset = 0) {
    return reinterpret_cast<T *>(buf_start() + headroom + offset);
  }

  void read(void *buf) {
    auto *seg = this;
    auto off = 0u;
    while (seg) {
      auto *src = seg->data<uint8_t>();
      std::memcpy(static_cast<uint8_t *>(buf) + off, src, seg->data_len);
      off += seg->data_len;
      seg = seg->next;
    }
  }

  mbuf *last_seg(size_t &size, uint32_t &segs) {
    auto *seg = this;
    while (seg->next) {
      size += seg->data_len;
      ++segs;
      seg = seg->next;
    }
    ++segs;
    size += seg->data_len;
    return seg;
  }

  static inline void merge(mbuf *&first, mbuf *&last, mbuf *seg, size_t &len,
                           uint32_t &segs) {
    if (!first) {
      first = seg;
      last = seg->last_seg(len, segs);
    } else {
      last->next = seg;
      last = seg->last_seg(len, segs);
    }
  }

  void write(const void *buf, size_t buf_len, size_t off = 0) {
    auto *src = static_cast<const uint8_t *>(buf);
    auto *seg = this;
    while (off >= seg->data_len) {
      off -= seg->data_len;
      seg = seg->next;
    }
    size_t buf_off = 0;
    while (seg && buf_off < buf_len) {
      auto len = seg->data_len - off;
      len = std::min(len, buf_len - buf_off);
      std::memcpy(seg->data<uint8_t>() + off, src + buf_off, len);
      buf_off += len;
      off = 0;
      seg = seg->next;
    }
  }

  template <typename T> T *prepend() {
    headroom -= sizeof(T);
    data_len += sizeof(T);
    return data<T>();
  }

  void adj(uint16_t len) {
    headroom += len;
    data_len -= len;
  }
};

struct obj_header {
  obj_header *next;
};

struct slab {
  slab *next;
  slab *prev;
  obj_header *freelist;
  uint32_t inuse;
  uint32_t padding;

  static void list_remove(slab *s) {
    s->prev->next = s->next;
    s->next->prev = s->prev;
  }

  slab() : next(nullptr), prev(nullptr), freelist(nullptr), inuse() {}
};

struct slab_cache {
  static constexpr size_t kDefaultCacheSize = 128;
  struct slab_list {
    slab head, tail;
    slab_list() : head(), tail() {
      head.next = &tail;
      tail.prev = &head;
    }

    void list_push(slab *s) {
      s->next = head.next;
      s->prev = &head;
      head.next->prev = s;
      head.next = s;
    }

    bool empty() const { return head.next == &tail; }

    slab *front() { return head.next; }
  };
  slab_list partial;
  slab_list full;
  size_t obj_size;

  slab_cache(size_t obj_size) : partial(), full(), obj_size(obj_size) {}
};

inline void mbuf_free(mbuf *buf);

using mbuf_ptr = std::unique_ptr<mbuf, decltype(&mbuf_free)>;
class slab_allocator {
public:
  static constexpr size_t kDefaultHeadroom = 128;
  static constexpr size_t kMaxDataLen = 1500 - protocol::defs::kHeaderMTUlen;
  static constexpr size_t kDefaultSize =
      kMaxDataLen + kDefaultHeadroom + sizeof(mbuf);
  static constexpr size_t kSlabSize = 2 * 1024 * 1024;

public:
  slab_allocator() : cache(kDefaultSize) { alloc_new_slab(cache); }

  mbuf *alloc_default(uint16_t data_len) {
    assert(data_len <= kMaxDataLen);
    if (cache.partial.empty())
      alloc_new_slab(cache);
    auto *s = cache.partial.front();
    auto *obj = s->freelist;
    s->freelist = obj->next;
    ++s->inuse;
    if (!s->freelist) {
      slab::list_remove(s);
      cache.full.list_push(s);
    }
    return new (obj)
        mbuf{this, nullptr, kDefaultSize, data_len, kDefaultHeadroom, false};
  }

  static uintptr_t virt_to_phys(void *vaddr) {
    const size_t PAGE_SIZE = sysconf(_SC_PAGESIZE);
    int fd;
    uint64_t entry;
    uintptr_t va = (uintptr_t)vaddr;
    off_t offset = (va / PAGE_SIZE) * sizeof(uint64_t);

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0)
      return RTE_BAD_IOVA;

    if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry)) {
      close(fd);
      assert(0);
      return RTE_BAD_IOVA;
    }
    close(fd);

    if (!(entry & (1ULL << 63))) {
      printf("not present\n");
      return RTE_BAD_IOVA;
    }

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    if (pfn == 0) {
      return RTE_BAD_IOVA;
    }
    return (pfn * PAGE_SIZE + (va % PAGE_SIZE));
  }

  void alloc_new_slab(slab_cache &c) {
    auto *region = mmap(nullptr, kSlabSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    assert(region != MAP_FAILED);
    auto *s = static_cast<slab *>(region);
    auto *base = reinterpret_cast<uint8_t *>(region) + sizeof(slab);
    s->freelist = new (base) obj_header;
    size_t space = kSlabSize - sizeof(slab);
    size_t off = 0;
    while (off + 2 * c.obj_size <= space) {
      auto *obj = reinterpret_cast<obj_header *>(base + off);
      obj->next = new (base + off + c.obj_size) obj_header;
      off += c.obj_size;
    }
    auto *obj = reinterpret_cast<obj_header *>(base + off);
    obj->next = nullptr;
    c.partial.list_push(s);
    assert(!cache.partial.empty());
  }

  void free_single_mbuf(mbuf *obj) {
    auto iptr = reinterpret_cast<intptr_t>(obj);
    auto *slb = reinterpret_cast<slab *>(iptr & ~(kSlabSize - 1));
    bool was_full = !slb->freelist;
    auto *hdr = reinterpret_cast<obj_header *>(obj);
    hdr->next = slb->freelist;
    slb->freelist = hdr;
    --slb->inuse;
    if (was_full) {
      slab::list_remove(slb);
      cache.partial.list_push(slb);
    }
  }

  void free_mbuf(mbuf *obj) {
    auto *obj_ptr = obj;
    while (obj_ptr) {
      auto *next = obj_ptr->next;
      free_single_mbuf(obj_ptr);
      obj_ptr = next;
    }
  }

  mbuf_ptr alloc_default_safe(uint16_t data_len) {
    auto *pkt = alloc_default(data_len);
    return mbuf_ptr(pkt, mbuf_free);
  }

  ~slab_allocator() {
    auto free_slabs = [](slab_cache::slab_list &list) {
      auto *s = list.head.next;
      while (s != &list.tail) {
        auto *next = s->next;
        munmap(s, kSlabSize);
        s = next;
      }
    };
    free_slabs(cache.partial);
    free_slabs(cache.full);
  }

private:
  slab_cache cache;
};

inline void mbuf_free(mbuf *buf) {
  assert(buf->sb);
  buf->sb->free_mbuf(buf);
}

inline mbuf_ptr mbuf_take_owner_ship(mbuf *pkt) {
  return mbuf_ptr(pkt, &mbuf_free);
}
