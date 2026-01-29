#include "packet_scheduler.h"
#include <cstdint>
#include <rte_cycles.h>

bool packet_scheduler::add_pkt(rte_mbuf *pkt) {
  if (ptr == buffer.size()) 
      do_send();
  buffer[ptr++] = pkt;
  return true;
}

uint16_t packet_scheduler::flush() {  
  if(ptr == 0)
      return 0;
  return do_send();
}

uint16_t packet_scheduler::do_send(){
    uint16_t sent = 0;
    do{
        sent += dev->tx_burst(buffer.data() + sent, ptr - sent);
    }while(sent < ptr);
    ptr = 0;
    return sent;
}
