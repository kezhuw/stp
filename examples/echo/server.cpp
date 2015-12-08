#include "stp.hpp"

#include "wild/module.hpp"
#include "wild/string.hpp"

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

using namespace stp;

int
main() {
    wild::module::Init();

    process::spawn([] {
        wild::Fd listener;
        std::error_condition error;
        std::tie(listener, error) = net::tcp::listen("tcp4://*:3000");
        if (error) {
            printf("fail to listen: %s\n", error.message().c_str());
            process::exit();
        }
        wild::Fd conn;
        std::string from;
        for (;;) {
            std::tie(conn, error) = net::tcp::accept(listener, &from);
            if (error) {
                printf("fail to accept: %s\n", error.message().c_str());
                break;
            }
            process::spawn([client = std::move(conn)] {
                byte_t buf[1024];
                size_t n;
                int err;
                for (;;) {
                    std::tie(n, err) = io::read(client.RawFd(), buf, sizeof buf);
                    if (err) {
                        printf("io::read(): %s\n", wild::os::strerror(err));
                        break;
                    }
                    std::tie(std::ignore, err) = io::write(client.RawFd(), buf, n);
                    if (err) {
                        printf("io::write(): %s\n", wild::os::strerror(err));
                        break;
                    }
                }
            });
        }
    });

    for (;;) {
        usleep(1000);
    }
}
