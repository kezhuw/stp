#pragma once

#include "types.hpp"

namespace stp {
namespace io {

std::tuple<size_t, int> read(int fd, byte_t *buf, size_t len);
std::tuple<size_t, int> write(int fd, const byte_t *data, size_t size);

std::tuple<size_t, int> read_full(int fd, byte_t *buf, size_t len);

}
}
