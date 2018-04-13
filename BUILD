COPTS = [
    "-Ideps",
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-Wconversion",
    "-Wshadow",
]

cc_library(
    name = "stp",
    srcs = [
        "src/context.cpp",
        "src/context.hpp",
        "src/main.cpp",
        "src/main.hpp",
        "src/event.cpp",
        "src/event.hpp",
        "src/sched.hpp",
        "src/fd.hpp",
        "src/io.cpp",
        "src/io.hpp",
        "src/os.cpp",
        "src/os.hpp",
        "src/SharedCallable.hpp",
        "src/tcp.cpp",
        "src/tcp.hpp",
        "src/types.hpp",
        "src/module.hpp",
        "src/process.cpp",
        "src/process.hpp",
        "src/time.cpp",
        "src/time.hpp",
    ] + select({
        "linux": ["src/fd_epoll.cpp"],
        "darwin": ["src/fd_kqueue.cpp"],
        "freebsd": ["src/fd_kqueue.cpp"],
    }),
    hdrs = [
        "stp.hpp",
    ],
    copts = COPTS,
    linkopts = [
        "-ldl",
        "-pthread",
    ],
    deps = [
        "//external:wild",
    ],
    visibility = ["//visibility:public"],
)

config_setting(
    name = "linux",
    values = {"host_cpu": "k8"},
)

config_setting(
    name = "darwin",
    values = {"host_cpu": "darwin"},
)

config_setting(
    name = "freebsd",
    values = {"host_cpu": "freebsd"},
)
