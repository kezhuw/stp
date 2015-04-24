#pragma once

#include "types.hpp"

#include <mutex>
#include <queue>
#include <functional>

namespace stp {
namespace coroutine {

void Spawn(std::function<void()> func, size_t addstack = 0);
void Timeout(uint64 msecs, std::function<void()> func, size_t addstack = 0);

void Sleep(uint64 msecs);

void Exit();

class Mutex {
public:

    Mutex() = default;

    void lock();
    void unlock();

    bool try_lock();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

private:
    std::queue<uintptr> _coroutines;
};

class Condition {
public:

    Condition() = default;
    ~Condition() = default;

    Condition(const Condition&) = delete;
    Condition& operator=(const Condition&) = delete;

    Condition(Condition&&) = delete;
    Condition& operator=(Condition&&) = delete;

    void wait(Mutex& locker);
    void wait(Mutex& locker, std::function<bool()> pred);

    void notify_one();
    void notify_all();

private:
    std::queue<uintptr> _coroutines;
};

}
}
