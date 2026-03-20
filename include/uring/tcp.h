#pragma once

#include <cstdio>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string_view>
#include <sys/socket.h>

namespace uring {
namespace tcp {
static constexpr char bbr_congestion[]{"bbr"};    

inline int get_tcp_stats(int fd, struct tcp_info *lk_tcp_info) {
  socklen_t len = sizeof(*lk_tcp_info);
  return getsockopt(fd, IPPROTO_TCP, TCP_INFO, lk_tcp_info, &len);
}

inline void print_tcp_info(FILE *stream, struct tcp_info *lk_tcp_info) {
  fprintf(stream, "Retransmits: %u\n", lk_tcp_info->tcpi_total_retrans);
  fprintf(stream, "RTT (us): %u\n", lk_tcp_info->tcpi_rtt);
  fprintf(stream, "RTT variance: %u\n", lk_tcp_info->tcpi_rttvar);
  fprintf(stream, "Cwnd (segs): %u\n", lk_tcp_info->tcpi_snd_cwnd);
  fprintf(stream, "Ssthresh: %u\n", lk_tcp_info->tcpi_snd_ssthresh);
  fprintf(stream, "Lost: %u\n", lk_tcp_info->tcpi_lost);
  fprintf(stream, "MSS Tx: %u\n", lk_tcp_info->tcpi_snd_mss);
  fprintf(stream, "Sacked: %u\n", lk_tcp_info->tcpi_sacked);
  fprintf(stream, "RX Ssthresh: %u\n", lk_tcp_info->tcpi_rcv_ssthresh);
  fprintf(stream, "RX RTT (us): %u\n", lk_tcp_info->tcpi_rcv_rtt);
  fprintf(stream, "RX: %lu\n", lk_tcp_info->tcpi_bytes_received);
}

inline int disable_nagle(int fd) {
  int flag = 1;
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));
}

inline int change_congestion_control(int fd, const std::string_view algo){
    return setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, algo.data(), algo.size());
}

} // namespace tcp

} // namespace uring
