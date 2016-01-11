#include "os.hpp"
#include "sched.hpp"
#include "process.hpp"

#include <exception>

namespace stp {
namespace os {

void exit(int code) {
    sched::stop(code);
    if (std::uncaught_exception()) {
        return;
    }
    process::exit();
}

}
}
