#pragma once

namespace stp {
namespace fd {

enum class Event : unsigned int {
    kRead       = 0,
    kWrite      = 1,
};

void wait(int fd, Event event);

}
}
