#pragma once

#include <memory>
#include <vector>

template <typename T> class indexable_queue {
public:
  indexable_queue(std::size_t size) : storage(size), size(size) {}
  T* enqueue(auto&& ...args){
      if(head == ((tail + 1) & (size - 1)))
          return nullptr;
      std::construct_at(&storage[tail], args...);
      auto *entry = &storage[tail];
      tail = (tail + 1) & (size - 1);
      return entry; 
  }

  bool full(){
      return ((tail + 1) & (size - 1)) == head;
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
      head = (head + 1) & (size - 1);
  }

  T& operator[](std::size_t i){
      return storage[(head + i) & (size - 1)];;
  }

private:
  std::vector<T> storage;
  std::size_t size;
  std::size_t head = 0, tail = 0;
};
