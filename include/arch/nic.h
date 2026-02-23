#pragma once

#include <cstdint>
struct nic{
    virtual uint64_t calc_rss_hash(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport) = 0;
    virtual void find_port_pair(uint32_t sip, uint32_t dip, uint16_t& sport, uint16_t& dport, uint16_t rtid) = 0;
    virtual ~nic() = default;
};
