#pragma once

#include <memory>
#include <vector>

template<typename T>
using Identity = T;

template <typename T, template <typename> typename P = Identity> class queue_base {
public:
  queue_base(std::size_t size) : storage(size), capacity(size), mask(size - 1) {}
  T* enqueue(auto&& ...args){
      if(head == ((tail + 1) & mask))
          return nullptr;
      std::construct_at(&storage[tail], args...);
      auto *entry = &storage[tail];
      tail = (tail + 1) & mask;
      return entry; 
  }

  bool full(){
      return ((tail + 1) & mask) == head;
  }

  bool empty(){
      return head == tail;
  }

  T* front(){
      if(head == tail)
          return nullptr;
      return &storage[head];
  }

  void pop_front(){
      head = (head + 1) & mask;
  }

  T& operator[](std::size_t i){
      return storage[(head + i) & mask];
  }

  std::size_t size() const{
      return (tail + capacity - head) & mask;
  }

protected:
  std::vector<T> storage;
  std::size_t capacity;
  std::size_t mask;
  P<std::size_t> head = 0, tail = 0;
};
