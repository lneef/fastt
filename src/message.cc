#include "message.h"
#include "debug.h"
#include <cstdint>


static const struct rte_mbuf_dynfield tsc_dynfield_desc = {
    .name = "ts",
    .size = sizeof(uint64_t),
    .align = alignof(uint64_t),
    .flags = 0};



int message::timestamp = -1;

int message::init(){
    timestamp = rte_mbuf_dynfield_register(&tsc_dynfield_desc);
    if(timestamp < 0){
        FASTT_LOG_DEBUG("Registering timestamp failed\n");
        return -1;
    }
    return 0;
}
