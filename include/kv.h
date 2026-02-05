#pragma once

#include "client.h"
#include "message.h"
#include "slot.h"
#include "util.h"
#include <cstdint>
#include <generic/rte_cycles.h>
#include <rte_lcore.h>
#include <rte_timer.h>

#include "kv_protocol.h"

inline void create_put_request(message *msg, int64_t key, int64_t val) {
  auto *kv_req = static_cast<kv_packet<kv_request> *>(msg->data());
  kv_req->payload.op = request_t::PUT;
  kv_req->payload.key = key;
  kv_req->payload.val = val;
}

inline void create_get_request(message *msg, int64_t key, int64_t id) {
  auto *kv_req = static_cast<kv_packet<kv_request> *>(msg->data());
  kv_req->pt = packet_t::SINGLE;
  kv_req->id = id;
  kv_req->payload.op = request_t::GET;
  kv_req->payload.key = key;
}

class kv_proxy {
public:
  kv_proxy(client_iface *ifc, connection *con)
      : ifc(ifc), con(con), completion_timeout(rte_get_timer_hz() / 1e4) {

  }

  transaction_slot* start_transaction(connection *con);
  void lookup(int64_t key, message *msg, int64_t id) { create_get_request(msg, key, id); };
  void acknowledge() { con->acknowledge_all(); }
  void finish_transaction(transaction_slot *slot);

  void poll_tx_completion() {  
    con->get_manager()->poll_single_connection(con, ready);
  }

  intrusive_list_t<transaction_slot> &completions() { return ready; }

  void flush() { ifc->flush(); }

private:
  static void poll_tx_completion_cb(rte_timer *timer, void *arg) {
    (void)timer;
    auto *kv = static_cast<kv_proxy *>(arg);
    kv->poll_tx_completion();
    // Assume one request
    // with one response
  }
  client_iface *ifc;
  connection *con;
  rte_timer timer;
  intrusive_list_t<transaction_slot> ready;
  uint64_t completion_timeout;
};
