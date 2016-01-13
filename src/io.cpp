#include "io.hpp"
#include "fd.hpp"
#include "process.hpp"

#include "wild/errno.hpp"
#include "wild/types.hpp"
#include "wild/io.hpp"

#include <assert.h>

namespace stp {
namespace io {

std::tuple<size_t, int>
read_full(int fd, byte_t *buf, size_t len) {
    size_t rd = 0;
    size_t n;
    int err;
    for (;;) {
        std::tie(n, err) = wild::io::Read(fd, buf, len);
        rd += n;
        len -= n;
        if (len == 0) {
            return std::make_tuple(rd, 0);
        }
        switch (err) {
        case EAGAIN:
            buf += n;
            fd::wait(fd, fd::Event::kRead);
            break;
        case EEOF:
            if (rd != 0) {
                // XXX unexpected EOF
            }
        default:
            return std::make_tuple(rd, err);
        }
    }
}

std::tuple<size_t, int>
read(int fd, byte_t *buf, size_t len) {
    size_t rd = 0;
    for (;;) {
        size_t n, err;
        std::tie(n, err) = wild::io::Read(fd, buf, len);
        rd += n;
        len -= n;
        if (len == 0) {
            return std::make_tuple(rd, 0);
        }
        switch (err) {
        case EAGAIN:
            if (rd == 0) {
                fd::wait(fd, fd::Event::kRead);
                break;
            }
            return std::make_tuple(rd, 0);
        default:
            return std::make_tuple(rd, err);
        }
    }
}

std::tuple<size_t, int>
write(int fd, const byte_t *data, size_t size) {
    size_t wr = 0;
    for (;;) {
        size_t n;
        int err;
        std::tie(n, err) = wild::io::Write(fd, data, size);
        wr += n;
        size -= n;
        if (size == 0) {
            return std::make_tuple(wr, 0);
        }
        switch (err) {
        case EAGAIN:
            data += n;
            fd::wait(fd, fd::Event::kWrite);
            break;
        default:
            return std::make_tuple(wr, err);
        }
    }
}

}
}
