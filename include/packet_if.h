#pragma once

#include "debug.h"
#include "dev.h"
#include "dpdk/allocator.h"
#include "slab_allocator.h"
#include "transport/protocol.h"
#include "util.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <generic/rte_cycles.h>
#include <netinet/in.h>
#include <random>
#include <rte_branch_prediction.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_ether.h>
#include <rte_gro.h>
#include <rte_ip.h>
#include <rte_ip4.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_memcpy.h>
#include <rte_memory.h>
#include <rte_udp.h>

struct packet_drop_sim {
  void set_rate(double rate) { threshold = rate * UINT32_MAX; }

  bool should_drop() { return threshold && dist(rng) < threshold; }

  packet_drop_sim()
      : rng(std::chrono::steady_clock::now().time_since_epoch().count()) {}

  uint32_t threshold = 0;
  std::mt19937 rng;
  std::uniform_int_distribution<uint32_t> dist{0, UINT32_MAX};
};

class packet_if {
  static void free_cb(void *, void *buf) {
    mbuf_free(reinterpret_cast<mbuf *>(buf));
  }
  static constexpr uint16_t kdefaultTTL = 64;
  static constexpr uint16_t kDefaultOutBurstSize = 32;

public:
  static constexpr uint16_t kDefaultInBurstSize = qpair::kDefaultInputBurstSize;

  packet_if(qpair *qp, std::shared_ptr<dpdk_allocator> pool, slab_allocator *sb,
            uint32_t sip, uint16_t port)
      : arp_table(), pool(pool), sb(sb), qp(qp), sip(sip) {
    rte_eth_macaddr_get(port, &smac);
    sim.set_rate(0.0);
    reo_off = sim.dist(sim.rng) & (UINT16_MAX - 1);
  }

  rte_udp_hdr *udp_header(rte_mbuf *msg, uint16_t sport, uint16_t dport,
                          uint16_t data_len) {
    auto *udp =
        rte_pktmbuf_mtod_offset(msg, rte_udp_hdr *, protocol::defs::kudpOffset);
#ifdef TEST_REORDERING
    udp->src_port = sport + (reo_off * should_reo);
    should_reo ^= 1;
#else
    udp->src_port = sport;
#endif
    udp->dst_port = dport;
    udp->dgram_cksum = 0;
    udp->dgram_len = rte_cpu_to_be_16(data_len + sizeof(rte_udp_hdr));
    msg->l4_len = sizeof(rte_udp_hdr);
    msg->data_len += msg->l4_len;
    msg->pkt_len += msg->l4_len;
    return udp;
  }

  void ip_header(rte_mbuf *msg, rte_udp_hdr *udp_header, uint32_t source,
                 uint32_t target, uint16_t data_len) {
    auto *ipv4 =
        rte_pktmbuf_mtod_offset(msg, rte_ipv4_hdr *, protocol::defs::kipOffset);
    ipv4->src_addr = source;
    ipv4->dst_addr = target;
    ipv4->fragment_offset = htons(RTE_IPV4_HDR_DF_FLAG);
    ipv4->next_proto_id = IPPROTO_UDP;
    ipv4->time_to_live = kdefaultTTL;
    ipv4->total_length =
        htons(data_len + sizeof(rte_udp_hdr) + sizeof(rte_ipv4_hdr));
    ipv4->hdr_checksum = 0;
    ipv4->version_ihl = RTE_IPV4_VHL_DEF;
    ipv4->type_of_service = 0;
    ipv4->packet_id = 0;
    msg->l3_len = sizeof(rte_ipv4_hdr);
    msg->data_len += msg->l3_len;
    msg->pkt_len += msg->l3_len;

    msg->ol_flags = 0;
    msg->ol_flags |=
        RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM | RTE_MBUF_F_TX_IPV4;
    udp_header->dgram_cksum = rte_ipv4_phdr_cksum(ipv4, msg->ol_flags);
  }

  void eth_header(rte_mbuf *msg, const rte_ether_addr &smac,
                  const rte_ether_addr &dmac) {
    auto *eth = rte_pktmbuf_mtod(msg, rte_ether_hdr *);
    rte_ether_addr_copy(&dmac, &eth->dst_addr);
    rte_ether_addr_copy(&smac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    msg->l2_len = sizeof(rte_ether_hdr);
    msg->data_len += msg->l2_len;
    msg->pkt_len += msg->l2_len;
  }

  void consume_pkt_mbuf(mbuf *pkt, transport_config &cfg) {
    switch (pkt->size_class) {
    case 1:
      consume_pkt_mbuf_zc(pkt, cfg);
      break;
    default:
      consume_pkt_mbuf_sc(pkt, cfg);
    }
  }

  void consume_pkt_mbuf_zc(mbuf *pkt, transport_config &cfg) {
    static_assert(
        (slab_allocator::kJumboHeadroom - sizeof(protocol::ft_header)) >=
            sizeof(dpdk_allocator::backend_data),
        "");
    auto *data = pkt->data<uint8_t>(sizeof(protocol::ft_header));
    auto iova = pkt->sb->get_iova(pkt, sizeof(protocol::ft_header));
    assert(iova != RTE_BAD_IOVA);
    assert(pkt->size_class == 1);
    assert(pkt->data_room == slab_allocator::kMaxJumboDataLen);
    assert(
        (reinterpret_cast<uintptr_t>(data) & (slab_allocator::kSlabSize - 1)) ==
        (iova & (slab_allocator::kSlabSize - 1)));
    auto *ext = rte_pktmbuf_alloc(pool->small);
    auto mbuf_data_len = pkt->data_len - sizeof(protocol::ft_header);
    dpdk_allocator::backend_data *shinfo;
    if (pkt->refcnt == 1) {
      shinfo = get_new_backend_data<dpdk_allocator::backend_data>(pkt);
      shinfo->fcb_opaque = pkt;
      shinfo->refcnt = 1;
      shinfo->free_cb = free_cb;
      ++pkt->refcnt;
    } else {
      shinfo = get_backend_data<dpdk_allocator::backend_data>(pkt);
      ++shinfo->refcnt;
    }
    assert(pkt->refcnt == 2);
    assert(shinfo->refcnt >= 1);
    rte_pktmbuf_attach_extbuf(ext, data, iova, mbuf_data_len, shinfo);
    ext->data_len = mbuf_data_len;
    auto *head = rte_pktmbuf_alloc(pool->pool);
    rte_pktmbuf_chain(head, ext);

    head->pkt_len = ext->data_len;
    auto head_payload_len = ext->data_len + sizeof(protocol::ft_header);
    assert(head->pkt_len == mbuf_data_len);
    rte_memcpy(rte_pktmbuf_mtod_offset(head, void *, protocol::defs::kftOffset),
               pkt->data<void>(), sizeof(protocol::ft_header));

    head->data_len = sizeof(protocol::ft_header);
    head->pkt_len += sizeof(protocol::ft_header);
    auto *udp = udp_header(head, cfg.transport_ports.sport,
                           cfg.transport_ports.dport, head_payload_len);
    ip_header(head, udp, sip, cfg.ip, head_payload_len);
    auto it = arp_table.find(cfg.ip);
    assert(it != arp_table.end());
    eth_header(head, smac, it->second);
    assert(head->pkt_len ==
           static_cast<size_t>(head_payload_len + head->l2_len + head->l3_len +
                               head->l4_len));
    qp->enqueue_pkt(head);
  }

  void consume_pkt_mbuf_sc(mbuf *pkt, transport_config &cfg) {

    auto *dpdk_mbuf = rte_pktmbuf_alloc(pool->get());
    dpdk_mbuf->data_len = pkt->data_len;
    dpdk_mbuf->pkt_len = pkt->data_len;
    std::memcpy(rte_pktmbuf_mtod_offset(dpdk_mbuf, uint8_t *,
                                        protocol::defs::kftOffset),
                pkt->data<uint8_t>(), pkt->data_len);
    auto *udp = udp_header(dpdk_mbuf, cfg.transport_ports.sport,
                           cfg.transport_ports.dport, pkt->data_len);
    ip_header(dpdk_mbuf, udp, sip, cfg.ip, pkt->data_len);
    auto it = arp_table.find(cfg.ip);
    assert(it != arp_table.end());
    eth_header(dpdk_mbuf, smac, it->second);
    assert(dpdk_mbuf->pkt_len ==
           static_cast<size_t>(pkt->data_len + dpdk_mbuf->l2_len +
                               dpdk_mbuf->l3_len + dpdk_mbuf->l4_len));
    qp->enqueue_pkt(dpdk_mbuf);
  }

  void add_mapping(uint32_t ip, rte_ether_addr &addr) {
    arp_table.emplace(ip, addr);
  }

  void broken_packet(rte_mbuf *pkt) {
    FASTT_LOG_DEBUG("Got broken packet\n");
    rte_pktmbuf_free(pkt);
  }

  bool check_ip_cksum(rte_mbuf *mbuf) {
    return !(mbuf->ol_flags & RTE_MBUF_F_RX_IP_CKSUM_BAD);
  }

  bool check_udp_cksum(rte_mbuf *mbuf) {
    return !(mbuf->ol_flags & RTE_MBUF_F_RX_L4_CKSUM_BAD);
  }

  bool check_ether(rte_mbuf *mbuf) {
    auto *eth = rte_pktmbuf_mtod(mbuf, rte_ether_hdr *);
    return eth->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  }

  void strip_ether_ip(rte_mbuf *mbuf, flow_tuple &ft) {
    auto *eth = rte_pktmbuf_mtod(mbuf, rte_ether_hdr *);
    auto *ip =
        rte_pktmbuf_mtod_offset(mbuf, rte_ipv4_hdr *, sizeof(rte_ether_hdr));
    add_mapping(ip->src_addr, eth->src_addr);
    ft.sip = ip->src_addr;
    ft.dip = ip->dst_addr;
  }

  void strip_udp(rte_mbuf *mbuf, flow_tuple &ft) {
    auto *udp = rte_pktmbuf_mtod_offset(mbuf, rte_udp_hdr *,
                                        protocol::defs::kudpOffset);
    ft.sport = udp->src_port;
    ft.dport = udp->dst_port;
  }

  rte_mbuf *consume_pkt(rte_mbuf *mbuf) {
    FASTT_LOG_DEBUG("Packet with %u segs of len %u\n", mbuf->nb_segs,
                    mbuf->pkt_len);
    if (!check_ether(mbuf)) {
      broken_packet(mbuf);
      return nullptr;
    }

    if (!check_ip_cksum(mbuf)) {
      broken_packet(mbuf);
      return nullptr;
    }

    if (!check_udp_cksum(mbuf)) {
      broken_packet(mbuf);
      return nullptr;
    }

    if (sim.should_drop()) {
      rte_pktmbuf_free(mbuf);
      return nullptr;
    }
    return mbuf;
  }

  void flush_out_buffer() { qp->flush(); }

  mbuf *strip_header_and_copy(rte_mbuf *msg, flow_tuple &ft) {
    static constexpr unsigned kDefaultLen = slab_allocator::kMaxDataLen + sizeof(protocol::ft_header);  
    strip_ether_ip(msg, ft);
    strip_udp(msg, ft);
    auto pkt_len = msg->pkt_len - protocol::defs::kftOffset;
    mbuf *head = nullptr;
    auto off = protocol::defs::kftOffset;
    if (likely(pkt_len <= kDefaultLen)) {
      // fast path for small packets
      head = sb->alloc_default(pkt_len - sizeof(protocol::ft_header));
      head->prepend<protocol::ft_header>();
      assert(head->data_len == pkt_len);
      auto *src = rte_pktmbuf_read(msg, off, pkt_len, head->data<void>());
      if (src != head->data<void>())
        rte_memcpy(head->data<void>(), src, pkt_len);
    } else {
      head = sb->alloc_large();
      head->prepend<protocol::ft_header>();
      assert(head->data_len >= pkt_len);
      auto *src = rte_pktmbuf_read(msg, off, pkt_len, head->data<void>());
      if (src != head->data<void>())
        rte_memcpy(head->data<void>(), src, pkt_len);
    }
    rte_pktmbuf_free(msg);
    return head;
  }

  void fetch_from_qpair(std::array<flow_tuple, kDefaultInBurstSize> &fts,
                        packet_vector<mbuf *, kDefaultInBurstSize> &mbufs, uint64_t ts) {
    uint16_t valid = 0, out = 0;
    assert(vec.i == 0);
    qp->rx_burst(vec);
    for (uint16_t i = 0; i < vec.i; ++i) {
      auto *pkt = consume_pkt(vec.pkts[i]);
      if (!pkt)
        continue;
      vec.pkts[valid++] = pkt;
    }
    vec.i = valid;
    assert(out == 0);
    for (auto *msg : vec) {
      mbufs.pkts[out] = strip_header_and_copy(msg, fts[out]);
      mbufs.pkts[out]->ts =ts; 
      ++out;
    }
    mbufs.i = out;
    assert(out == valid);
    vec.clear();
  }

  uint32_t get_sip() const { return sip; }

private:
  packet_drop_sim sim;
  flow_table<uint32_t, rte_ether_addr> arp_table;
  rte_ether_addr smac;
  std::shared_ptr<dpdk_allocator> pool;
  slab_allocator *sb;
  qpair *qp;
  packet_vector<rte_mbuf *, kDefaultInBurstSize> vec;
  uint32_t sip;
  uint16_t reo_off;
#ifdef TEST_REORDERING
  uint16_t should_reo = 0;
#endif
};
