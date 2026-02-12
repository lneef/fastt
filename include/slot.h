#pragma once

#include "connection.h"
#include "message.h"
#include "transport/transport.h"
#include <cstdint>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_lcore.h>

enum class slot_state {
  COMPLETED,
  RUNNING,
};

struct slot {
  uint32_t id;
  connection *con;

  slot(uint32_t id, connection *con) : id(id), con(con) {}
  slot() = default;

  struct {
    bool send(message *msg) { return slt->con->send_pkt(msg, true, true); }

    slot *slt;
  } tx_if{this};
};
