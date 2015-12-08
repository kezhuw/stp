#pragma once

#include "wild/types.hpp"

namespace stp {

using namespace wild::types;

namespace process {

struct process_t {
    constexpr explicit process_t(uint32 value = 0)
        : _value(value) {}

    explicit operator bool() const {
        return _value != 0;
    }

    uint32 Value() const {
        return _value;
    }

private:
    uint32 _value;
};

struct session_t {
    constexpr explicit session_t(uint32 value = 0)
        : _value(value) {}

    explicit operator bool() const {
        return _value != 0;
    }

    uint32 Value() const {
        return _value;
    }

private:
    uint32 _value;
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

using process_t = process::process_t;
using session_t = process::session_t;

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
        return std::hash<stp::process_t>()(std::get<stp::process_t>(request))
             + std::hash<stp::session_t>()(std::get<stp::session_t>(request));
    }
};

}
