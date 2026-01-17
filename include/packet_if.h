#pragma once

#include "log.h"
#include "message.h"
#include "packet_scheduler.h"
#include "util.h"
#include <absl/container/flat_hash_map.h>
#include <cstdint>
#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_ip4.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_udp.h>

class packet_if {
  static constexpr uint16_t kdefaultTTL = 64;

public:
  packet_if(packet_scheduler *scheduler, uint32_t sip, uint16_t port) : scheduler(scheduler), sip(sip) {
    rte_eth_macaddr_get(port, &smac);
  }

  rte_udp_hdr *udp_header(message *msg, uint16_t sport, uint16_t dport) {
    auto *udp = msg->move_headroom<rte_udp_hdr>();
    udp->src_port = rte_cpu_to_be_16(sport);
    udp->dst_port = rte_cpu_to_be_16(dport);
    udp->dgram_cksum = 0;
    udp->dgram_len = rte_cpu_to_be_16(msg->pkt_len);
    msg->l4_len = sizeof(rte_udp_hdr);
    return udp;
  }

  void ip_header(message *msg, rte_udp_hdr *udp_header, uint32_t source,
                 uint32_t target) {
    auto *ipv4 = msg->move_headroom<rte_ipv4_hdr>();
    ipv4->src_addr = source;
    ipv4->dst_addr = target;
    ipv4->fragment_offset = 0;
    ipv4->next_proto_id = IPPROTO_UDP;
    ipv4->time_to_live = kdefaultTTL;
    ipv4->total_length = rte_cpu_to_be_16(msg->pkt_len);
    ipv4->hdr_checksum = 0;
    ipv4->version_ihl = RTE_IPV4_VHL_DEF;
    ipv4->type_of_service = 0;
    ipv4->packet_id = 0;
    msg->l3_len = sizeof(rte_ipv4_hdr);

    msg->ol_flags = 0;
    msg->ol_flags |=
        RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_UDP_CKSUM | RTE_MBUF_F_TX_IPV4;
    udp_header->dgram_cksum = rte_ipv4_phdr_cksum(ipv4, msg->ol_flags);
  }

  void eth_header(message *msg, const rte_ether_addr &smac,
                  const rte_ether_addr &dmac) {
    auto *eth = msg->move_headroom<rte_ether_hdr>();
    rte_ether_addr_copy(&dmac, &eth->dst_addr);
    rte_ether_addr_copy(&smac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    msg->l2_len = sizeof(rte_ether_hdr);
  }

  void consume_pkt(message *msg, uint16_t sport, const con_config &tcon_config) {  
    auto *udp = udp_header(msg, sport, tcon_config.port);
    ip_header(msg, udp, sip, tcon_config.ip);
    assert(arp.contains(tcon_config.ip));
    eth_header(msg, smac, arp[tcon_config.ip]);
    dump_pkt(msg, msg->len());
    scheduler->add_pkt(static_cast<rte_mbuf *>(msg));
  }

  void consume_for_retransmission(message* msg){
      scheduler->add_pkt(msg);
  }

  void add_mapping(uint32_t ip, rte_ether_addr &addr) {
    auto [it, inserted] = arp.emplace(ip, rte_ether_addr());
    if (inserted)
      rte_ether_addr_copy(&addr, &it->second);
  }

  void broken_packet(rte_mbuf *pkt) { 
      FASTT_LOG_DEBUG("Got broken packet\n");
      FASTT_DUMP_PKT(static_cast<message*>(pkt), pkt->data_len);
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

  void strip_ether(rte_mbuf *mbuf) {
    rte_pktmbuf_adj(mbuf, sizeof(rte_ether_hdr));
  }

  void strip_ip(rte_mbuf *mbuf, flow_tuple &ft) {
    auto *ip = rte_pktmbuf_mtod(mbuf, rte_ipv4_hdr *);
    ft.sip = ip->src_addr;
    ft.dip = ip->dst_addr;
    rte_pktmbuf_adj(mbuf, sizeof(rte_ipv4_hdr));

  }

  void strip_udp(rte_mbuf *mbuf, flow_tuple &ft) {
    auto *udp = rte_pktmbuf_mtod(mbuf, rte_udp_hdr *);
    ft.sport = udp->src_port;
    ft.dport = udp->dst_port;
    rte_pktmbuf_adj(mbuf, sizeof(rte_udp_hdr));
  }

  message *consume_pkt(rte_mbuf *mbuf, flow_tuple &ft) {
    if (check_ether(mbuf))
      strip_ether(mbuf);
    else {
      broken_packet(mbuf);
      return nullptr;
    }
    if (check_ip_cksum(mbuf))
      strip_ip(mbuf, ft);
    else {
      broken_packet(mbuf);
      return nullptr;
    }
    if (check_udp_cksum(mbuf))
      strip_udp(mbuf, ft);
    else {
      broken_packet(mbuf);
      return nullptr;
    }
    return static_cast<message *>(mbuf);
  }

private:
  absl::flat_hash_map<uint32_t, rte_ether_addr> arp;
  rte_ether_addr smac;
  packet_scheduler *scheduler;
  uint32_t sip;
};
