#pragma once

#include "types.hpp"

#include <wild/Any.hpp>
#include <wild/types.hpp>
#include <wild/SpinLock.hpp>
#include <wild/with_lock.hpp>
#include <wild/utility.hpp>

#include <cassert>

#include <queue>
#include <utility>
#include <type_traits>

namespace stp {
namespace message {

using Content = wild::Any;

struct Message {
    process_t source;
    session_t session;
    Content content;
};

Message* create(process::process_t source, process::session_t session, message::Content content);

void destroy(Message *m);

// one reader, multiple writer, nonblocking.
// After nullptr returned from take(), reader
// should not call take() again util notified
// by writer.
class Mailbox {
public:

    bool push(Message *msg) {
        WITH_LOCK(_mutex) {
            if (_wait) {
                _wait = false;
                _in.push(msg);
                return true;
            }
            _out.push(msg);
        }
        return false;
    }

    Message *take() {
        if (_in.empty()) {
            WITH_LOCK(_mutex) {
                if (_out.empty()) {
                    _wait = true;
                    return nullptr;
                }
                using std::swap;
                swap(_in, _out);
            }
        }
        assert(!_in.empty());
        return wild::take(_in);
    }

private:

    wild::SpinLock _mutex;
    bool _wait = false;
    std::queue<Message*> _in;
    std::queue<Message*> _out;
};

}
}
