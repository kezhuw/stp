#include "sched.hpp"
#include "module.hpp"
#include "process.hpp"

#include <wild/module.hpp>
#include <wild/spinlock.hpp>
#include <wild/with_lock.hpp>

#include <thread>
#include <condition_variable>

namespace  {

using namespace stp;
using namespace stp::sched;

class ProcessQueue {
public:

    void Push(Process *p) {
        bool empty;
        WITH_LOCK(_mutex) {
            empty = _queue.Empty();
            _queue.Push(p);
        }
        // XXX reader may not wait on this, harmless.
        if (empty) {
            _notEmpty.notify_one();
        }
    }

    Process *Take() {
        WITH_LOCK(_mutex) {
            while (_queue.Empty()) {
                _notEmpty.wait(_mutex);
            }
            return _queue.Take();
        }
    }

private:
    wild::SpinLock _mutex;
    std::condition_variable_any _notEmpty;
    process::ForwardList _queue;
};

class Worker {
public:

    Worker() {
        _thread = std::thread(std::mem_fn(&Worker::Run), this);
    }

    void Run() {
        for (;;) {
            Process *p = _queue.Take();
            process::Resume(p);
        }
    }

    void Queue(Process *p) {
        _queue.Push(p);
    }

private:
    ProcessQueue _queue;
    std::thread _thread;
};

class Scheduler {
public:

    void Start(size_t n) {
        _workers.reserve(n);
        while (n-- > 0) {
            _workers.push_back(new Worker);
        }
        printf("%lu schedulers\n", _workers.size());
    }

    void Schedule(Process *p) {
        uintptr randval = reinterpret_cast<uintptr>(p) + reinterpret_cast<uintptr>(&p);
        size_t index = ((randval >> 4) + (randval >> 16))%_workers.size();
        Worker *worker = _workers[index];
        worker->Queue(p);
    }

private:
    std::vector<Worker*> _workers;
};

Scheduler& scheduler() {
    static Scheduler gScheduler;
    return gScheduler;
}

void init() {
    scheduler().Start(8);
}

wild::module::Definition sched(module::STP, "stp:sched", init, module::Order::Sched);

}

namespace stp {
namespace sched {

void Schedule(Process *p) {
    scheduler().Schedule(p);
}

}
}
