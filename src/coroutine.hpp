#pragma once

#include "types.hpp"

#include <mutex>
#include <queue>
#include <functional>

namespace stp {
namespace coroutine {

class Coroutine;

Coroutine *Task(std::function<void()>func, size_t addstack = 0);
void Spawn(std::function<void()> func, size_t addstack = 0);

void Wakeup(Coroutine *co, uintptr result = 0);

void Sleep(wild::uint64 msecs);
void Timeout(uint64 msecs, std::function<void()> func, size_t addstack = 0);

Coroutine *Running();

void Exit(uintptr result);
uintptr Join(Coroutine *co);

class ForwardList {
public:

    void Push(Coroutine *);
    Coroutine *Take();

    Coroutine *Front() const;
    bool Empty() const;

private:
    Coroutine *_head = nullptr;
    Coroutine *_tail = nullptr;
};

class Mutex {
public:

    void lock();
    void unlock();

    bool try_lock();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

private:
    ForwardList _coroutines;
};

}
}
