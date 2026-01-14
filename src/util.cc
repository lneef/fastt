#include "util.h"

bool operator==(const flow_tuple& lhs, const flow_tuple& rhs) {
  return lhs.sip == rhs.sip && lhs.dip == rhs.dip && lhs.sport == rhs.sport && lhs.dport == rhs.dport;
}
