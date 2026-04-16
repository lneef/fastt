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
#include <rte_dev.h>
#include <rte_ethdev.h>
#include <sys/mman.h>
#include <unistd.h>
class slab_allocator;

struct mbuf {
  slab_allocator *sb;
  mbuf *next;
  uint32_t data_room : 28;
  uint32_t size_class : 4;
  uint16_t data_len;
  uint16_t headroom : 10;
  uint16_t refcnt : 6;
  uint64_t ts;

  mbuf() = default;
  mbuf(slab_allocator *sb, mbuf *next, uint32_t data_room, uint32_t size_class,
       uint16_t data_len, uint16_t headroom)
      : sb(sb), next(next), data_room(data_room), size_class(size_class),
        data_len(data_len), headroom(headroom), refcnt(1) {}

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

template <typename T, typename F>
static void parse_mbuf(mbuf &dgram, F &&consumer) {
  auto off = 0u;
  while (off < dgram.data_len)
    off += consumer(dgram.data<T>(off));
}

struct obj_header {
  obj_header *next;
};

struct alignas(64) slab {
  slab *next;
  slab *prev;
  uintptr_t iova;
  obj_header *freelist;
  uint32_t inuse;
  uint32_t padding;

  static void list_remove(slab *s) {
    s->prev->next = s->next;
    s->next->prev = s->prev;
  }

  slab() : next(nullptr), prev(nullptr), iova(0), freelist(nullptr), inuse() {}
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
  unsigned top = 0;
  unsigned color = 0;
  std::array<obj_header *, kDefaultCacheSize> local_cache;

  slab_cache(size_t obj_size) : partial(), full(), obj_size(obj_size) {}
};

template <typename B> B *get_new_backend_data(mbuf *data) {
  auto *hdroom_start = reinterpret_cast<uint8_t *>(data) + sizeof(mbuf);
  return new (hdroom_start) B;
}

template <typename B> B *get_backend_data(mbuf *data) {
  auto *hdroom_start = reinterpret_cast<uint8_t *>(data) + sizeof(mbuf);
  return reinterpret_cast<B *>(hdroom_start);
}

inline void mbuf_free(mbuf *buf);

using mbuf_ptr = std::unique_ptr<mbuf, decltype(&mbuf_free)>;
class slab_allocator {
  static constexpr unsigned kSizeClassCnt = 2;
  static constexpr unsigned kDefaultPrefill = 4;
  static constexpr unsigned kLargePrefill = 1;
  static constexpr unsigned kPageCacheSize = 4;

public:
  static constexpr size_t kDefaultHeadroom = 20;
  static constexpr size_t kMaxDataLen = 1500 - protocol::defs::kHeaderMTUlen;
  static constexpr size_t kDefaultSize =
      kMaxDataLen + kDefaultHeadroom + sizeof(mbuf);
  static constexpr size_t kSlabSize = 2 * 1024 * 1024;
  static constexpr size_t kJumboHeadroom = 96;
  static constexpr size_t kMaxJumboDataLen =
      9001 - protocol::defs::kHeaderMTUlen;
  static constexpr size_t kDefaultJumboSize =
      kMaxJumboDataLen + kJumboHeadroom + sizeof(mbuf) + 7;
  static_assert(kDefaultJumboSize % 64 == 0, "");
  using dma_map_t = int (*)(void *, size_t, unsigned, uint64_t, size_t);
  using dma_unmap_t = int (*)(void *, uint64_t, size_t);

public:
  slab_allocator(dma_map_t map = nullptr, dma_unmap_t unmap = nullptr,
                 unsigned default_prefill_thres = kDefaultPrefill,
                 unsigned large_prefill_thres = kLargePrefill)
      : caches{slab_cache(kDefaultSize), slab_cache(kDefaultJumboSize)},
        map(map), unmap(unmap) {
    for (unsigned i = 0; i < default_prefill_thres; ++i)
      alloc_new_slab(caches[0]);
    for (unsigned i = 0; i < large_prefill_thres; ++i)
      alloc_new_slab(caches[1]);
  }

  template <unsigned cl, size_t mbuf_size, size_t hdroom, bool iova>
  mbuf *alloc(uint16_t data_len) {
    auto &cache = caches[cl];
    obj_header *obj = nullptr;
    if (cache.top) {
      obj = cache.local_cache[--cache.top];
    } else {
      if (cache.partial.empty())
        alloc_new_slab<iova>(cache);
      auto *s = cache.partial.front();
      obj = s->freelist;
      assert(obj);
      s->freelist = obj->next;
      ++s->inuse;
      if (!s->freelist) {
        slab::list_remove(s);
        cache.full.list_push(s);
        assert(cache.partial.front() != s);
      }
    }
    return new (obj) mbuf{this, nullptr, mbuf_size, cl, data_len, hdroom};
  }

  mbuf *alloc_default(uint16_t data_len) {
    assert(data_len <= kMaxDataLen);
    return alloc<0, kMaxDataLen, kDefaultHeadroom, false>(data_len);
  }

  mbuf *alloc_large() {
    return alloc<1, kMaxJumboDataLen, kJumboHeadroom, true>(kMaxJumboDataLen);
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
      return RTE_BAD_IOVA;
    }
    close(fd);

    if (!(entry & (1ULL << 63)))
      return RTE_BAD_IOVA;

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    if (pfn == 0)
      return RTE_BAD_IOVA;
    return (pfn * PAGE_SIZE + (va % PAGE_SIZE));
  }

  template <bool iova = false> void alloc_new_slab(slab_cache &c) {
    if (pcache.top == 0)
      fill_cache();
    auto *region = pcache.pages[--pcache.top];
    auto *s = new (region) slab();
    if constexpr (iova) {
      // prefault, MAP_POPULATE may fail
      s->iova = virt_to_phys(region);
      assert(s->iova != RTE_BAD_IOVA);
      if (map)
        map(region, kSlabSize, 1, s->iova, kSlabSize);
    }

    size_t off = c.color;
    auto *base = reinterpret_cast<uint8_t *>(region) + sizeof(slab);
    s->freelist = new (base + off) obj_header;
    size_t space = kSlabSize - sizeof(slab);
    while (off + 2 * c.obj_size <= space) {
      auto *obj = reinterpret_cast<obj_header *>(base + off);
      obj->next = new (base + off + c.obj_size) obj_header;
      off += c.obj_size;
    }
    auto *obj = reinterpret_cast<obj_header *>(base + off);
    obj->next = nullptr;
    c.color = (c.color + 64) & 1023;
    c.partial.list_push(s);
  }

  uintptr_t get_iova(mbuf *obj, unsigned off) const {
    auto iptr = reinterpret_cast<intptr_t>(obj);
    auto *slb = reinterpret_cast<slab *>(iptr & ~(kSlabSize - 1));
    return slb->iova +
           (obj->data<uint8_t>(off) - reinterpret_cast<uint8_t *>(slb));
  }

  void free_single_mbuf(mbuf *obj) {
    assert(obj->size_class < kSizeClassCnt);
    auto &cache = caches[obj->size_class];
    auto iptr = reinterpret_cast<intptr_t>(obj);
    auto *hdr = reinterpret_cast<obj_header *>(obj);
    if (cache.top < slab_cache::kDefaultCacheSize) {
      hdr->next = nullptr;
      cache.local_cache[cache.top++] = hdr;
    } else {
      auto *slb = reinterpret_cast<slab *>(iptr & ~(kSlabSize - 1));
      bool was_full = !slb->freelist;
      hdr->next = slb->freelist;
      slb->freelist = hdr;
      --slb->inuse;
      if (was_full) {
        slab::list_remove(slb);
        cache.partial.list_push(slb);
      }
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

  mbuf_ptr alloc_large_safe() {
    auto *jumbo_pkt = alloc_large();
    return mbuf_ptr(jumbo_pkt, mbuf_free);
  }

  ~slab_allocator() {
    auto free_slabs = [&](slab_cache::slab_list &list) {
      auto *s = list.head.next;
      while (s != &list.tail) {
        auto *next = s->next;
        if (s->iova && unmap)
          unmap(s, s->iova, kSlabSize);
        munmap(s, kSlabSize);
        s = next;
      }
    };
    for (auto &cache : caches) {
      free_slabs(cache.partial);
      free_slabs(cache.full);
    }
  }

private:
  void fill_cache() {
    auto *pre = static_cast<uint8_t *>(
        mmap(nullptr, kSlabSize * (kPageCacheSize - pcache.top), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0));
    assert(pre != MAP_FAILED);
    for (unsigned i = pcache.top; i < kPageCacheSize; ++i) {
      pcache.pages[pcache.top++] = pre;
      pre += kSlabSize;
      assert((reinterpret_cast<uintptr_t>(pcache.pages[pcache.top - 1]) & (kSlabSize - 1)) ==
             0);
    }
  }
  std::array<slab_cache, kSizeClassCnt> caches;
  struct {
    std::array<void *, kPageCacheSize> pages;
    size_t top = 0;
  } pcache;
  const dma_map_t map;
  const dma_unmap_t unmap;
};

static constexpr int16_t sign_extend(uint16_t val) {
  struct {
    int16_t v : 6;
  } f;
  f.v = val;
  return f.v;
}

inline void mbuf_free(mbuf *buf) {
  assert(sign_extend(buf->refcnt) > 0);
  --buf->refcnt;
  if (buf->refcnt != 0)
    return;
  assert(buf->sb);
  buf->sb->free_mbuf(buf);
}

inline mbuf_ptr mbuf_take_owner_ship(mbuf *pkt) {
  return mbuf_ptr(pkt, &mbuf_free);
}
