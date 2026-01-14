#include "message.h"
#include "log.h"
#include <cstdint>


static const struct rte_mbuf_dynfield tsc_dynfield_desc = {
    .name = "ts",
    .size = sizeof(uint64_t),
    .align = alignof(uint64_t),
    .flags = 0};

static const struct rte_mbuf_dynfield con_dynfield_desc = {
    .name = "con",
    .size = sizeof(connection*),
    .align = alignof(void*),
    .flags = 0};

int message::timestamp = -1;
int message::con_ptr = -1;

int message::init(){
    timestamp = rte_mbuf_dynfield_register(&tsc_dynfield_desc);
    if(timestamp < 0){
        FASTT_LOG_DEBUG("Registering timestamp failed\n");
        return -1;
    }
    con_ptr = rte_mbuf_dynfield_register(&con_dynfield_desc);
    if(con_ptr < 0){
        FASTT_LOG_DEBUG("Refistering con_ptr failed\n");
        return -1;
    }
    return 0;
}
