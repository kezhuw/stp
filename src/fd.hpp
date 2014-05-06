#pragma once

namespace stp {
namespace fd {

enum class Event : unsigned int {
    Read        = 0,
    Write       = 1,
};

void Wait(int fd, Event event);

}
}
