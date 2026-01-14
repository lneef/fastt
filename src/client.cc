#include "client.h"
#include "connection.h"
#include "message.h"
#include "util.h"

message* client_iface::recv_message(connection* con){
    message* msg;
    if(!con->has_ready_message()){
        manager.fetch_from_device();
        con->acknowledge_all();
    } 
    if(con->receive_message(&msg, 1))
        return msg;
    return nullptr;
}

connection* client_iface::open_connection(const con_config& target, rte_ether_addr& dmac){
    manager.add_mac(target.ip, dmac);
    return manager.open_connection(scon_config, target);
}
