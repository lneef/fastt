#pragma once

#include "client.h"
#include "message.h"
#include <cstdint>

static constexpr uint16_t payload_offset = 0;
enum class packet_t: uint8_t{
    SINGLE = 0, BATCH = 1,
};

enum class request_t: uint8_t{
    GET = 0, PUT = 1, DELETE = 2,
};

enum class response_t: uint8_t{
    SUCCESS, FAILURE,
};

struct[[gnu::packed]] kv_packet_base{
    packet_t pt;
    uint64_t id;
};

struct [[gnu::packed]] kv_request {
    request_t op;
    int64_t key;
    int64_t val;
}; 

struct [[gnu::packed]] kv_completion {
    response_t reponse;
    int64_t val;
};

template<typename T>
struct [[gnu::packed]] kv_packet : public kv_packet_base{
    T payload;
};

template<typename T>
struct [[gnu::packed]] kv_batch : public kv_packet_base{
    uint32_t elems;
    T elements[];
};

inline void create_put_request(message* msg, int64_t key, int64_t val){
    auto* kv_req = static_cast<kv_packet<kv_request>*>(msg->data());
    kv_req->payload.op = request_t::PUT;
    kv_req->payload.key = key;
    kv_req->payload.val = val;
}

inline void create_get_request(message* msg, int64_t key){
    auto* kv_req = static_cast<kv_packet<kv_request>*>(msg->data());
    kv_req->pt = packet_t::SINGLE;
    kv_req->payload.op = request_t::GET;
    kv_req->payload.key = key;
}

struct transaction_proxy;

class kv_proxy{
    public:
        kv_proxy(client_iface* ifc, connection* con): ifc(ifc), con(con){}
 
        std::unique_ptr<transaction_proxy> start_transaction(connection* con, transaction_queue& q);
        void lookup(int64_t key, message* msg){
            create_get_request(msg, key);
        };
        void acknowledge() { con->acknowledge_all(); }
        void finish_transaction(transaction_proxy* proxy);
        void poll_tx_completion(){
            con->get_manager()->poll_single_connection(con);
        }
        void flush(){ ifc->flush(); }
    private:
            client_iface* ifc;
            connection* con;

};
