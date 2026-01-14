#include "server.h"
#include "message.h"
#include <cstdint>

bool server_iface::send_message(connection* con, message* msg, uint16_t len){
    msg->set_size(len);
    return con->send_message(msg, len);
}

void server_iface::flush(){
    manager.flush();
}

connection* server_iface::accept(){
    return manager.accept_connection();
}
