#pragma once




#include "kv_protocol.h"
#include "message.h"
#include "server.h"
#include "connection.h"

class dispatcher{
    public:
        dispatcher(server_iface& sifc): sifc(sifc) {};
    void poll(){
        sifc.poll([](message* msg, connection* con){
                auto* hdr = msg->data<kv::kv_packet_base>();
                });
    }

    void schedule_task(){

    }

    private:
    server_iface& sifc;

};
