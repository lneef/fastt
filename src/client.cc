#include "client.h"
#include "transport/session.h"
#include "transport/slot.h"
#include "message.h"
#include "util.h"

message* client_iface::recv_message(client_slot* con){
    message* msg;
    if(!con->poll()){
        manager.fetch_from_device();
        con->acknowledge();
    } 
    if(con->receive_msg(&msg))
        return msg;
    return nullptr;
}

client_session* client_iface::open_session(const con_config& target, rte_ether_addr& dmac){
    manager.add_mac(target.ip, dmac);
    return manager.open_session(scon_config, target);
}
