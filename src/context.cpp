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

std::tuple<void*, size_t> mstack(size_t addstack) {
    if (addstack != 0) {
        size_t stackmask = gPageSize - 1;
        addstack = (addstack + stackmask) & (~stackmask);
    }
    size_t stacksize = 8*gPageSize + addstack;
    assert(stacksize % gPageSize == 0);

    size_t index = stacksize/gPageSize;
    if (index < std::extent<decltype(gStacks)>::value) {
        struct stacklist& sl = gStacks[index];
        WITH_LOCK(sl.Mutex) {
            struct link *stk = sl.First;
            if (stk) {
                sl.First = stk->_link;
                auto stackbase = reinterpret_cast<void*>(stk);
                return std::make_tuple(stackbase, stacksize);
            }
        }
    }

    void *lowPage = mmap(nullptr, stacksize+2*gPageSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(lowPage != MAP_FAILED);
    void *stackbase = static_cast<void*>(static_cast<byte_t*>(lowPage) + gPageSize);
    void *highPage = static_cast<void*>(static_cast<byte_t*>(stackbase) + stacksize);
    mprotect(lowPage, gPageSize, PROT_NONE);
    mprotect(highPage, gPageSize, PROT_NONE);
    return std::make_tuple(stackbase, stacksize);
}

void munstack(void *stackbase, size_t stacksize) {
    assert(stacksize % gPageSize == 0);
    size_t index = stacksize/gPageSize;
    if (index < std::extent<decltype(gStacks)>::value) {
        struct link *stk = reinterpret_cast<struct link *>(stackbase);
        struct stacklist& sl = gStacks[index];
        WITH_LOCK(sl.Mutex) {
            stk->_link = sl.First;
            sl.First = stk;
        }
        return;
    }

    void *lowPage = static_cast<void*>(static_cast<byte_t*>(stackbase) - gPageSize);
    int err = munmap(lowPage, stacksize+2*gPageSize);
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

    static Context *New(void (*func)(void*), void *arg, size_t addstack) {
        if (auto ctx = allocContext()) {
            new (ctx) Context(func, arg, addstack);
            return ctx;
        }
        return new Context(func, arg, addstack);
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

    Context(void (*func)(void *), void *arg, size_t addstack) {
        std::tie(_stackbase, _stacksize) = mstack(addstack);

        int err = getcontext(&_ucontext);
        assert(err == 0);
        (void)err;

#if defined(__FreeBSD__) || defined(__APPLE__)
using stack_pointer_t = char *;
#else
using stack_pointer_t = void *;
#endif
        _ucontext.uc_stack.ss_sp = static_cast<stack_pointer_t>(_stackbase);
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
    static wild::FreeList<Context> gFrees;

    static thread_local wild::FreeList<Context> tFrees;

    static Context *allocContext() {
        if (auto ctx = tFrees.take()) {
            return ctx;
        }
        WITH_LOCK(gMutex) {
            return gFrees.take();
        }
    }

    static void deallocContext(Context *ctx) {
        if (tFrees.size() >= 5000) {
            WITH_LOCK(gMutex) {
                gFrees.push(ctx);
            }
            return;
        }
        tFrees.push(ctx);
    }
};

wild::SpinLock Context::gMutex;
wild::FreeList<Context> Context::gFrees;

thread_local wild::FreeList<Context> Context::tFrees;

Context* New() {
    return Context::New();
}

Context* New(void (*func)(void *), void *arg, size_t addstack) {
    return Context::New(func, arg, addstack);
}

void Switch(Context *current, Context *to) {
    Context::Switch(current, to);
}

void Delete(Context *ctx) {
    Context::Delete(ctx);
}

}
}
