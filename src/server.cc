#include "server.h"
#include "message.h"
#include "transport/slot.h"
#include <cstdint>

bool server_iface::send_message(server_slot* con, message* msg, uint16_t len){
    msg->set_size(len);
    return con->send_message(msg, 0);
}

void server_iface::flush(){
    manager.flush();
}

slot* server_iface::accept(){
    return manager.accept_slot();
}
