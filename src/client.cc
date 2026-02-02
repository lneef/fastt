#include "client.h"
#include "connection.h"
#include "util.h"

connection *client_iface::open_connection(const con_config &target,
                                          rte_ether_addr &dmac) {
  manager.add_mac(target.ip, dmac);
  return manager.open_connection(scon_config, target);
}
