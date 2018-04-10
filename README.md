# Coroutine based application level multiprocess library for C++
stp explores a concurrency pattern in C++ by embedding cooperative coroutines in lightweight processes.


## Motivation
I dislike [thread pool][], and hate [asynchronous][].

I like [goroutine][] in Go and [lightweight process][] in Erlang.

I love [coroutine][] in [Lua][].


## Differentiate between multithreading and cooperative coroutines
Multithreading implies multiple [preemptive][] and potential parallel execution flows.

Cooperative coroutines are multiple [cooperative][] and not parallel execution flows.


## Usage
```cpp
// Echo server

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
```


## TODO
[ ] How to bootstrap stp and application code ?
[ ] Update to C++ 17
[ ] Split exported and internal head files
[ ] Channel support, mailbox is a process level channel
[ ] Nonblocking file operations base on aio


## Inspiration
* [skynet][]: A lightweight online game framework


## License
The MIT License (MIT). See [LICENSE](LICENSE) for the full license text.


[thread pool]: https://en.wikipedia.org/wiki/Thread_pool
[asynchronous]: https://en.wikipedia.org/wiki/Asynchrony_(computer_programming)
[goroutine]: https://tour.golang.org/concurrency/1
[coroutine]: https://en.wikipedia.org/wiki/Coroutine
[preemptive]: https://en.wikipedia.org/wiki/Preemption_(computing)
[cooperative]: https://en.wikipedia.org/wiki/Cooperative_multitasking
[lightweight process]: https://en.wikipedia.org/wiki/Erlang_(programming_language)#Concurrency_and_distribution_orientation
[Lua]: https://www.lua.org/start.html
[skynet]: https://github.com/cloudwu/skynet
