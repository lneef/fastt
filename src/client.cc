#include "client.h"
#include "connection.h"
#include "util.h"
#include <cstdint>

connection *client_iface::open_connection(const con_config &target,
                                          uint16_t rtid, rte_ether_addr &dmac) {
  manager.add_mac(target.ip, dmac);
  return manager.open_connection(scon_config.port, target.port, scon_config.ip,
                                 target.ip, rtid);
}
