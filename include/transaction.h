#include "client.h"
#include "connection.h"
#include "kv.h"
#include "message.h"
#include "queue.h"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <rte_mbuf_core.h>

struct transaction{
    uint64_t id;
    uint64_t to;
    message* msg;
};

class transaction_queue{
    public:
        transaction_queue(std::size_t size): queue(std::bit_ceil(size)){}

        transaction* enqueue(){
            if(queue.full())
                return nullptr;
            return queue.enqueue(id++, 0, nullptr);
        }

        transaction& front(){
            return *queue.front();
        }

        void cleanup(){
            while(!queue.empty() && front().msg){
                queue.pop_front();
                ++least_in_queue;
            }
        }

        void pop_front(){
            queue.pop_front();
            ++least_in_queue;
        }

        transaction& operator[](std::size_t i){
            return queue[i - least_in_queue];
        }
    private:
        uint64_t id = 0;
        uint64_t least_in_queue = 0;
        queue_base<transaction> queue;
};

struct response_proxy{
    transaction_queue& q;
    kv_proxy* kp;
    connection* con;
    transaction *t;

    void wait_for_completion(message*& resp){
        message* msg;
        while(!t->msg){
            msg = kp->recv_completion(con);
            auto* hdr = rte_pktmbuf_mtod(msg, kv_packet<kv_completion>*);
            q[hdr->id].msg = msg;
        }
        con->acknowledge_all();
        q.pop_front();
        resp = msg;
    }

};

