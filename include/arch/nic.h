#pragma once

struct nic{
    unsigned n_queues;

    nic(unsigned n_queues) : n_queues(n_queues) {}

    virtual void update(int port) = 0;
    virtual unsigned best_queue() const = 0;
    virtual ~nic() = default;
};
