// 依存ゼロの最小テストハーネス。
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace meguri_test {

struct TestCase {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int failure_count = 0;

struct Registrar {
    Registrar(const char* name, std::function<void()> func) {
        registry().push_back({name, std::move(func)});
    }
};

inline int run_all() {
    int failed_cases = 0;
    for (const auto& test : registry()) {
        const int before = failure_count;
        try {
            test.func();
        } catch (const std::exception& e) {
            ++failure_count;
            std::printf("  [EXCEPTION] %s: %s\n", test.name.c_str(), e.what());
        }
        const bool ok = failure_count == before;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", test.name.c_str());
        if (!ok) ++failed_cases;
    }
    std::printf("%zu test(s), %d failed\n", registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace meguri_test

#define TEST_CASE(name)                                                              \
    static void test_func_##name();                                                  \
    static const meguri_test::Registrar registrar_##name{#name, test_func_##name};   \
    static void test_func_##name()

#define CHECK(expr)                                                                  \
    do {                                                                             \
        if (!(expr)) {                                                               \
            ++meguri_test::failure_count;                                            \
            std::printf("  CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        }                                                                            \
    } while (0)

#define CHECK_EQ(a, b)                                                               \
    do {                                                                             \
        const auto va = (a);                                                         \
        const auto vb = (b);                                                         \
        if (!(va == vb)) {                                                           \
            ++meguri_test::failure_count;                                            \
            std::printf("  CHECK_EQ failed at %s:%d: %s == %s (%lld != %lld)\n",     \
                        __FILE__, __LINE__, #a, #b, static_cast<long long>(va),      \
                        static_cast<long long>(vb));                                 \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                        \
    do {                                                                             \
        const double va = (a);                                                       \
        const double vb = (b);                                                       \
        if (!(va - vb < (eps) && vb - va < (eps))) {                                 \
            ++meguri_test::failure_count;                                            \
            std::printf("  CHECK_NEAR failed at %s:%d: %s ~= %s (%f != %f)\n",       \
                        __FILE__, __LINE__, #a, #b, va, vb);                         \
        }                                                                            \
    } while (0)
