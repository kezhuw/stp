#include "stp.hpp"

#include "wild/module.hpp"
#include "wild/string.hpp"

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

extern "C" void
stp_main() {
    wild::Fd listener;
    std::error_condition error;
    std::tie(listener, error) = stp::net::tcp::listen("tcp4://*:3000");
    if (error) {
        printf("fail to listen: %s\n", error.message().c_str());
        stp::process::exit();
    }
    wild::Fd conn;
    std::string from;
    for (;;) {
        std::tie(conn, error) = stp::net::tcp::accept(listener, &from);
        if (error) {
            printf("fail to accept: %s\n", error.message().c_str());
            break;
        }
        stp::process::spawn([client = std::move(conn)] {
            stp::byte_t buf[1024];
            size_t n;
            int err;
            for (;;) {
                std::tie(n, err) = stp::io::read(client.RawFd(), buf, sizeof buf);
                if (err) {
                    printf("stp::io::read(): %s\n", wild::os::strerror(err));
                    break;
                }
                std::tie(std::ignore, err) = stp::io::write(client.RawFd(), buf, n);
                if (err) {
                    printf("stp::io::write(): %s\n", wild::os::strerror(err));
                    break;
                }
            }
        });
    }
}

int
main(int argc, const char *args[]) {
    return stp::main(argc, args);
}
