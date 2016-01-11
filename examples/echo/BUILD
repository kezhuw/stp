cc_binary(
    name = "client",
    srcs = ["client.cpp"],
    deps = ["//:stp", "@wild//:wild"],
    copts = [
        "-Iexternal",
        "-std=c++1y",
    ],
)

cc_binary(
    name = "server",
    srcs = ["server.cpp"],
    deps = ["//:stp", "@wild//:wild"],
    copts = [
        "-Iexternal",
        "-std=c++1y",
    ],
)
