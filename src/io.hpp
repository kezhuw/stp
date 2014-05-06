#pragma once

#include "types.hpp"

namespace stp {
namespace io {

std::tuple<size_t, int> Read(int fd, byte_t *buf, size_t len);
std::tuple<size_t, int> Write(int fd, byte_t *data, size_t size);

std::tuple<size_t, int> ReadFull(int fd, byte_t *buf, size_t len);

}
}
