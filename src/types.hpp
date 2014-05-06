#pragma once

#include <wild/types.hpp>

namespace stp {

namespace process {

class Process;

struct process_t {
    constexpr explicit process_t(wild::uint32 value = 0)
        : _value(value) {}

    explicit operator bool() const {
        return _value != 0;
    }

    wild::uint32 Value() const {
        return _value;
    }

private:
    wild::uint32 _value;
};

struct session_t {
    constexpr explicit session_t(wild::uint32 value = 0)
        : _value(value) {}

    explicit operator bool() const {
        return _value != 0;
    }

    wild::uint32 Value() const {
        return _value;
    }

private:
    wild::uint32 _value;
};

inline bool operator==(process_t const& a, process_t const& b) {
    return a.Value() == b.Value();
}

inline bool operator!=(process_t const& a, process_t const& b) {
    return !(a == b);
}

inline bool operator==(session_t const& a, session_t const& b) {
    return a.Value() == b.Value();
}

inline bool operator!=(session_t const& a, session_t const& b) {
    return !(a == b);
}

}

namespace coroutine { class Coroutine; }

namespace message { struct Message; }

}

namespace stp {

using uint = wild::uint;

using int32 = wild::int32;
using uint32 = wild::uint32;

using int64 = wild::int64;
using uint64 = wild::uint64;

using intptr = wild::intptr;
using uintptr = wild::uintptr;

using byte_t = wild::byte_t;
using size_t = wild::size_t;
using string = wild::string;

using func_t = wild::func_t;
using pointer_t = wild::pointer_t;

using process_t = process::process_t;
using session_t = process::session_t;

using Process = process::Process;
using Message = message::Message;
using Coroutine = coroutine::Coroutine;

}

#include <tuple>
#include <functional>

namespace std {

template<>
struct hash<stp::process_t> {
    typedef stp::process_t argument_type;
    typedef std::size_t value_type;

    value_type operator()(argument_type const& key) const {
        return std::hash<decltype(key.Value())>()(key.Value());
    }
};

template<>
struct hash<stp::session_t> {
    typedef stp::session_t argument_type;
    typedef std::size_t value_type;

    value_type operator()(argument_type const& key) const {
        return std::hash<decltype(key.Value())>()(key.Value());
    }
};

template<>
struct hash<std::tuple<stp::process_t, stp::session_t>> {
    typedef std::tuple<stp::process_t, stp::session_t> argument_type;
    typedef std::size_t value_type;

    value_type operator()(argument_type const& request) const {
        return std::hash<stp::process_t>()(std::get<0>(request))
             + std::hash<stp::session_t>()(std::get<1>(request));
    }
};

}
