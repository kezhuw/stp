#include "stp.hpp"
#include "timer.hpp"
#include "net.hpp"
#include "io.hpp"

#include <wild/module.hpp>
#include <wild/string.hpp>

#include <cstdio>
#include <cstdlib>

#include <unistd.h>

using namespace stp;

int
main() {
    wild::module::Init();

    process::Spawn([] {
        wild::Fd listener;
        std::error_condition error;
        std::tie(listener, error) = net::tcp::Listen("tcp4://*:3000");
        if (error) {
            printf("fail to listen: %s\n", error.message().c_str());
            process::Exit();
        }
        wild::Fd conn;
        std::string from;
        for (;;) {
            std::tie(conn, error) = net::tcp::Accept(listener, &from);
            if (error) {
                printf("fail to accept: %s\n", error.message().c_str());
                break;
            }
            process::Spawn([client = std::move(conn)] {
                byte_t buf[1024];
                size_t n;
                int err;
                for (;;) {
                    std::tie(n, err) = io::Read(client.RawFd(), buf, sizeof buf);
                    if (err) {
                        printf("io::Read(): %s\n", wild::os::strerror(err));
                        break;
                    }
                    std::tie(std::ignore, err) = io::Write(client.RawFd(), buf, n);
                    if (err) {
                        printf("io::Write(): %s\n", wild::os::strerror(err));
                        break;
                    }
                }
            });
        }
    });

    for (;;) {
        timer::UpdateTime();
        usleep(1000);
    }
}
