#include "client.h"
#include "connection.h"
#include "queue.h"
#include "transport/slot.h"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <rte_mbuf_core.h>

struct transaction_handle{
    transaction_slot* slot;
};

class transaction_queue{
    public:
        transaction_queue(std::size_t size): queue(std::bit_ceil(size)){}

        transaction_handle* enqueue(transaction_slot* slot){
            if(queue.full())
                return nullptr;
            return queue.enqueue(slot);
        }

        transaction_handle& front(){
            return *queue.front();
        }


        void pop_front(){
            queue.pop_front();
            ++least_in_queue;
        }

        transaction_handle& operator[](std::size_t i){
            return queue[i - least_in_queue];
        }
    private:
        uint64_t least_in_queue = 0;
        queue_base<transaction_handle> queue;
};

struct transaction_proxy{
    transaction_queue& q;
    connection* con;
    transaction_handle *t;

    transaction_handle& wait_for_completion(){
        while(!t->slot->rx_if.has_incoming_messages()){
            con->get_manager()->fetch_from_device();
            con->process_incoming();
        }
        q.pop_front();
        return *t;
    }

};

