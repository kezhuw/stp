#include "tcp.hpp"

#include "fd.hpp"
#include "process.hpp"

#include <wild/likely.hpp>
#include <wild/types.hpp>
#include <wild/io.hpp>
#include <wild/string.hpp>
#include <wild/ScopeGuard.hpp>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

#include <algorithm>
#include <tuple>
#include <system_error>

#define memclr(ptr, len)    memset((ptr), 0, (len))

union sockaddr_all {
    struct sockaddr s;
    struct sockaddr_in v4;
    struct sockaddr_in6 v6;
};

#define SOCK_FLAGS      (SOCK_NONBLOCK | SOCK_CLOEXEC)

// error code
namespace {

enum class net_errc {
    none                = 0,
    illegal_address     = 1,
    all_address_failed  = 2,
    no_match_addrinfo   = 3,
    ambiguous_address   = 4,
};

class stp_net_category : public std::error_category {
public:

    virtual const char* name() const noexcept override {
        return "stp_net";
    }

    virtual std::string message(int condition) const noexcept override {
        switch (static_cast<net_errc>(condition)) {
        case net_errc::illegal_address:
            return "illegal address format";
        case net_errc::all_address_failed:
            return "all address failed";
        case net_errc::no_match_addrinfo:
            return "no match addrinfo";
        case net_errc::ambiguous_address:
            return "ambiguous address";
        default:
            break;
        }
        return "unknown error code";
    }
};

class stp_gai_category : public std::error_category {
public:

    virtual const char* name() const noexcept override {
        return "stp_gai";
    }

    virtual std::string message(int condition) const noexcept override {
        const char *strerr = gai_strerror(condition);
        if (strerr == nullptr) {
            return "unknown error";
        }
        return strerr;
    }
};

const std::error_category&
net_category() {
    static stp_net_category category;
    return category;
}

const std::error_category&
gai_category() {
    static stp_gai_category category;
    return category;
}

std::error_condition
make_error_condition(net_errc err) {
    return std::error_condition(static_cast<int>(err), net_category());
}

}

namespace std {

template<>
struct is_error_condition_enum<net_errc> : public std::true_type {};

}

namespace {

using namespace stp;

string addressString(union sockaddr_all const& addr) {
    int family = addr.s.sa_family;
    void *inetx_addr = (family == AF_INET) ? (void*)(&addr.v4.sin_addr) : (void*)(&addr.v6.sin6_addr);
    char tmp[std::max(INET_ADDRSTRLEN, INET6_ADDRSTRLEN)];
    if (LIKELY(::inet_ntop(family, inetx_addr, tmp, static_cast<socklen_t>(sizeof tmp)) != nullptr)) {
        string header = (family == AF_INET) ? "tcp4://" : "tcp6://";
        return header + tmp;
    }
    fprintf(stderr, "[%s:%s:%d] inet_ntop() fail to fetch address: %s\n",
                    __FILE__, __func__, __LINE__, wild::os::strerror(errno));
    return "";
}

#define TCP_PREFIX      "tcp://"
#define TCP4_PREFIX     "tcp4://"
#define TCP6_PREFIX     "tcp6://"

// ":6666"
// "*:6060"
// "tcp://*:6666"
// "tcp6://:6666"
// "tcp4://12.34.0.39:6666"
size_t
_trim(char *addr, int *family) {
    *family = AF_UNSPEC;
    if (strncmp(addr, TCP_PREFIX, sizeof(TCP_PREFIX)-1) == 0) {
        return sizeof(TCP_PREFIX)-1;
    } else if (strncmp(addr, TCP4_PREFIX, sizeof(TCP4_PREFIX)-1) == 0) {
        *family = AF_INET;
        return sizeof(TCP4_PREFIX)-1;
    } else if (strncmp(addr, TCP6_PREFIX, sizeof(TCP6_PREFIX)-1) == 0) {
        *family = AF_INET6;
        return sizeof(TCP6_PREFIX)-1;
    }
    return 0;
}

bool
_parse(char *addr, size_t len, int *family, char **host, char **port) {
    size_t n = _trim(addr, family);
    addr += n;
    len -= n;

    char *sep = static_cast<char*>(memrchr(addr, ':', len));
    if (sep == NULL) return false;

    char *beg = NULL;
    char *end = NULL;
    if (addr[0] == '[') {
        end = sep-1;
        if (*end != ']') {
            return false;
        }
        beg = addr+1;
    } else {
        beg = addr;
        end = sep;
    }
    assert(beg <= end);
    if ((beg == end) || ((beg+1 == end) && beg[0] == '*')) {
        *host = NULL;
    } else {
        *end = '\0';
        *host = beg;
    }
    *port = sep+1;
    return true;
}

int
_option(int fd) {
    int reuse = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

int
_getsockerr(int fd) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == -1) {
        err = errno;
    }
    return err;
}

std::error_condition
_getaddrinfo(const char *addr, struct addrinfo **res, int flag) {
    size_t len = strlen(addr);
    char copy[len+1];
    memcpy(copy, addr, len);
    copy[len] = '\0';

    char *host = NULL;
    char *port = NULL;
    int family = AF_UNSPEC;
    if (!_parse(copy, len, &family, &host, &port)) {
        return std::error_condition(net_errc::illegal_address);
    }

    struct addrinfo hint;
    memclr(&hint, sizeof hint);
    hint.ai_family = family;
    hint.ai_socktype = SOCK_STREAM;
    hint.ai_protocol = IPPROTO_TCP;
    hint.ai_flags = flag | AI_NUMERICHOST | AI_NUMERICSERV;

    *res = NULL;
    int err = getaddrinfo(host, port, &hint, res);
    switch (err) {
    case 0:
        break;
    case EAI_SYSTEM:
        return std::system_category().default_error_condition(errno);
    default:
        return gai_category().default_error_condition(err);
    }
    if (*res == nullptr) {
        return std::error_condition(net_errc::no_match_addrinfo);
    }
    return std::error_condition();
}

void
_closen(int n, int fds[]) {
    while (n--) {
        close(fds[n]);
    }
}

}

namespace stp {
namespace net { namespace tcp {

using byte_t = wild::byte_t;
using string = wild::string;
namespace io = wild::io;

std::tuple<wild::Fd, std::error_condition>
listen(const char *addr) {
    struct addrinfo *res;
    if (auto err = _getaddrinfo(addr, &res, AI_PASSIVE)) {
        return std::make_tuple(wild::Fd(), err);
    }

    int sockets[2];
    int nsocket = 0;

    int lastErrno = 0;
    for (struct addrinfo *ai = res; ai != NULL && nsocket < 2; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_FLAGS, ai->ai_protocol);
        if (fd != -1) {
            if (_option(fd) == 0
             && bind(fd, ai->ai_addr, ai->ai_addrlen) == 0
             && ::listen(fd, SOMAXCONN) == 0) {
                sockets[nsocket++] = fd;
                continue;
            }
            lastErrno = errno;
            close(fd);
        }
        lastErrno = errno;
    }
    freeaddrinfo(res);

    if (nsocket == 0) {
        assert(lastErrno != 0);
        return std::make_tuple(wild::Fd(), std::system_category().default_error_condition(lastErrno));
    } else if (nsocket > 1) {
        _closen(nsocket, sockets);
        return std::make_tuple(wild::Fd(), std::error_condition(net_errc::ambiguous_address));
    }

    return std::make_tuple(wild::Fd(sockets[0]), std::error_condition());
}

std::tuple<wild::Fd, std::error_condition>
connect(const char *addr) {
    struct addrinfo *res;
    if (auto error = _getaddrinfo(addr, &res, 0)) {
        return std::make_tuple(wild::Fd(), error);
    }

    SCOPE_EXIT {
        freeaddrinfo(res);
    };

    int lastErrno = 0;
    struct addrinfo *ai = res;
    do {
        int rawfd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_FLAGS, ai->ai_protocol);
        if (rawfd < 0) {
            lastErrno = errno;
            continue;
        }
        wild::Fd wildfd(rawfd);
        int e = ::connect(rawfd, ai->ai_addr, ai->ai_addrlen);
        if (e != 0 && errno != EINPROGRESS) {
            lastErrno = errno;
            continue;
        }
        fd::wait(rawfd, fd::Event::kWrite);
        lastErrno = _getsockerr(rawfd);
        if (lastErrno) {
            continue;
        }
        return std::make_tuple(std::move(wildfd), std::error_condition());
    } while ((ai = ai->ai_next));
    assert(lastErrno != 0);
    return std::make_tuple(wild::Fd(), std::system_category().default_error_condition(lastErrno));
}

std::tuple<wild::Fd, std::error_condition>
accept(const wild::Fd& fd, string *from) {
    int listener = fd.RawFd();
    union sockaddr_all addr;
    for (;;) {
        socklen_t len = sizeof(addr);
        int conn = ::accept4(listener, &addr.s, &len, SOCK_FLAGS);
        if (conn < 0) {
            int err = errno;
            switch (err) {
            case EAGAIN:
                fd::wait(listener, fd::Event::kRead);
            case EINTR:
                continue;
            default:
                return std::make_tuple(wild::Fd(), std::system_category().default_error_condition(err));
            }
        }
        if (from) {
            *from = addressString(addr);
        }
        return std::make_tuple(wild::Fd(conn), std::error_condition());
    }
}

} }
}
