#pragma once

#include <cstdint>
struct nic{
    unsigned n_queues;

    nic(unsigned n_queues) : n_queues(n_queues) {}

    virtual void update(int port) = 0;
    virtual uint64_t calc_rss_hash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) = 0;
    virtual void find_port_pair(uint32_t sip, uint32_t dip, uint16_t& sport, uint16_t& dport, uint16_t rtid) = 0;
    virtual unsigned best_queue() const = 0;
    virtual ~nic() = default;
};
