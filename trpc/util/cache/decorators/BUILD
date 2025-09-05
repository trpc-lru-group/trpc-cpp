# Description: trpc-cpp.

licenses(["notice"])

package(default_visibility = ["//visibility:public"])

cc_library(
    name = "fifo_cache",
    hdrs = ["fifo_cache.h"],
    deps = [
        "//trpc/util/cache",
        "//trpc/util/concurrency:lightly_concurrent_hashmap",
    ],
)

cc_test(
    name = "fifo_cache_test",
    srcs = ["fifo_cache_test.cc"],
    deps = [
        ":fifo_cache",
        "//trpc/util/cache/impl:basic_cache",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "lru_cache",
    hdrs = ["lru_cache.h"],
    deps = [
        "//trpc/util/cache",
        "//trpc/util/concurrency:lightly_concurrent_hashmap",
    ],
)

cc_test(
    name = "lru_cache_test",
    srcs = ["lru_cache_test.cc"],
    deps = [
        ":lru_cache",
        "//trpc/util/cache/impl:basic_cache",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "sharded_lru_cache",
    hdrs = ["sharded_lru_cache.h"],
    deps = [
        "//trpc/util/cache",
        "//trpc/util/concurrency:lightly_concurrent_hashmap",
    ],
)

cc_test(
    name = "sharded_lru_cache_test",
    srcs = ["sharded_lru_cache_test.cc"],
    deps = [
        ":sharded_lru_cache",
        "//trpc/util/cache/impl:basic_cache",
        "//trpc/util/cache/decorators:lru_cache",
        "@com_google_googletest//:gtest",
        "@com_google_googletest//:gtest_main",
    ],
)