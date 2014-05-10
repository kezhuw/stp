#include "context.hpp"

#include <wild/freelist.hpp>
#include <wild/spinlock.hpp>
#include <wild/with_lock.hpp>

#include <unistd.h>
#include <ucontext.h>
#include <sys/mman.h>

#include <assert.h>

namespace {

using namespace stp;
using namespace stp::context;

struct link {
    struct link *_link;
};

struct stacklist {
    wild::SpinLock Mutex;
    struct link *First;
} gStacks[32];

const size_t gPageSize = (size_t)sysconf(_SC_PAGESIZE);

void* mstack(size_t& stacksize) {
    if (stacksize != 0) {
        size_t stackmask = gPageSize - 1;
        stacksize = (stacksize + stackmask) & (~stackmask);
    }
    stacksize += 8*gPageSize;

    size_t index = stacksize/gPageSize;
    if (index < std::extent<decltype(gStacks)>::value) {
        struct stacklist& sl = gStacks[index];
        WITH_LOCK(sl.Mutex) {
            struct link *stk = sl.First;
            if (stk) {
                sl.First = stk->_link;
                return static_cast<void*>(stk);
            }
        }
    }

    void *headPage = mmap(nullptr, stacksize+2*gPageSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    void *stackbase = static_cast<void*>(static_cast<byte_t*>(headPage) + gPageSize);
    void *tailPage = static_cast<void*>(static_cast<byte_t*>(stackbase) + stacksize);
    mprotect(headPage, gPageSize, PROT_NONE);
    mprotect(tailPage, gPageSize, PROT_NONE);
    return stackbase;
}

void munstack(void *stackbase, size_t stacksize) {
    size_t index = stacksize/gPageSize;
    if (index < std::extent<decltype(gStacks)>::value) {
        struct link *stk = static_cast<struct link *>(stackbase);
        struct stacklist& sl = gStacks[index];
        WITH_LOCK(sl.Mutex) {
            stk->_link = sl.First;
            sl.First = stk;
        }
        return;
    }

    void *headPage = static_cast<void*>(static_cast<byte_t*>(stackbase) - gPageSize);
    int err = munmap(headPage, stacksize+2*gPageSize);
    assert(err == 0);
    (void)err;
}

}

namespace stp {
namespace context {

class Context {
public:

    static Context *New() {
        if (auto ctx = allocContext()) {
            new (ctx) Context();
            return ctx;
        }
        return new Context();
    }

    static Context *New(size_t stacksize, void (*func)(void*), void *arg) {
        if (auto ctx = allocContext()) {
            new (ctx) Context(stacksize, func, arg);
            return ctx;
        }
        return new Context(stacksize, func, arg);
    }

    static void Delete(Context *ctx) {
        ctx->~Context();
        deallocContext(ctx);
    }

    static void Switch(Context *current, Context *to) {
        swapcontext(&current->_ucontext, &to->_ucontext);
    }

private:

    Context()
        : _stackbase(nullptr)
        , _stacksize(0) {}

    Context(size_t stacksize, void (*func)(void *), void *arg) {
        _stackbase = mstack(stacksize);
        _stacksize = stacksize;

        int err = getcontext(&_ucontext);
        assert(err == 0);
        (void)err;

        _ucontext.uc_stack.ss_sp = _stackbase;
        _ucontext.uc_stack.ss_size = _stacksize;
        _ucontext.uc_stack.ss_flags = 0;
        _ucontext.uc_link = nullptr;

        makecontext(&_ucontext, reinterpret_cast<void(*)()>(func), 1, arg);
    }

    ~Context() {
        if (_stackbase) {
            munstack(_stackbase, _stacksize);
        }
    }

    void * _stackbase;
    size_t _stacksize;
    ucontext_t _ucontext;

    static wild::SpinLock gMutex;
    static wild::FreeListT<Context> gFrees;

    static thread_local wild::FreeListST<Context> tFrees;

    static Context *allocContext() {
        if (auto ctx = tFrees.Take()) {
            return ctx;
        }
        WITH_LOCK(gMutex) {
            return gFrees.Take();
        }
    }

    static void deallocContext(Context *ctx) {
        if (tFrees.Size() >= 5000) {
            WITH_LOCK(gMutex) {
                gFrees.Free(ctx);
            }
            return;
        }
        tFrees.Free(ctx);
    }
};

wild::SpinLock Context::gMutex;
wild::FreeListT<Context> Context::gFrees;

thread_local wild::FreeListST<Context> Context::tFrees;

Context* New() {
    return Context::New();
}

Context* New(size_t stacksize, void (*func)(void *), void *arg) {
    return Context::New(stacksize, func, arg);
}

void Switch(Context *current, Context *to) {
    Context::Switch(current, to);
}

void Delete(Context *ctx) {
    Context::Delete(ctx);
}

}
}
