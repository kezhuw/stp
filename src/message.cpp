#include "message.hpp"

#include <wild/freelist.hpp>
#include <wild/spinlock.hpp>
#include <wild/with_lock.hpp>

#include <cstdlib>

namespace {

using namespace stp;

wild::SpinLock gMutex;
wild::FreeListT<Message> gFreeList;

thread_local wild::FreeListST<Message> tFreeList;

Message *
allocMessage() {
    auto m = tFreeList.Take();
    if (m == nullptr) {
        WITH_LOCK(gMutex) {
            m = gFreeList.Take();
        }
        if (m == nullptr) {
            m = static_cast<Message*>(malloc(sizeof(Message)));
        }
    }
    return m;
}

void
deallocMessage(Message *m) {
    if (tFreeList.Size() >= 10000) {
        WITH_LOCK(gMutex) {
            gFreeList.Free(m);
        }
        return;
    }
    tFreeList.Free(m);
}

}

namespace stp {
namespace message {

Message *New(process_t source, session_t session, message::Kind kind, message::Code code, const message::Content& content) {
    auto m = allocMessage();
    return new (m) Message{source, session, kind, code, content, nullptr};
}

void Delete(Message *msg) {
    msg->~Message();
    deallocMessage(msg);
}

}
}
