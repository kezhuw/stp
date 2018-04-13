#include "process.hpp"
#include "sched.hpp"
#include "main.hpp"

#include "wild/module.hpp"
#include "wild/ScopeGuard.hpp"

namespace stp {

int
main(std::function<void()> entry) {
    wild::module::Init();

    stp::process::spawn([&entry] {
        SCOPE_EXIT {
            sched::stop(0);
        };
        entry();
    });
    return stp::sched::serve();
}

}
