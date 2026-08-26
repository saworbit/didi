#include <iostream>
#include <vector>
#include <functional>
#include <string>
#include "didi/common/logger.hpp"

struct TestCase {
    std::string name;
    std::function<void()> test_fn;
};

static std::vector<TestCase>& getTestRegistry() {
    static std::vector<TestCase> s_tests;
    return s_tests;
}

void registerTest(const std::string& name, std::function<void()> fn) {
    getTestRegistry().push_back({name, fn});
}

#define TEST(suite, name) \
    void test_##suite##_##name(); \
    struct Register_##suite##_##name { \
        Register_##suite##_##name() { \
            registerTest(#suite "." #name, test_##suite##_##name); \
        } \
    } g_register_##suite##_##name; \
    void test_##suite##_##name()

#define ASSERT_TRUE(cond) \
    if (!(cond)) { \
        std::cerr << "Assertion failed: (" #cond ") at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw std::runtime_error("Assertion failed: " #cond); \
    }

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))

int main(int argc, char* argv[]) {
    didi::Logger::instance().setLevel(didi::LogLevel::Warn);

    std::cout << "========================================" << std::endl;
    std::cout << " Running Didi Native MCP Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int failed = 0;

    for (const auto& test : getTestRegistry()) {
        std::cout << "[ RUN      ] " << test.name << std::endl;
        try {
            test.test_fn();
            std::cout << "\033[32m[       OK ]\033[0m " << test.name << std::endl;
            passed++;
        } catch (const std::exception& e) {
            std::cout << "\033[31m[  FAILED  ]\033[0m " << test.name << " (" << e.what() << ")" << std::endl;
            failed++;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << " Results: " << passed << " passed, " << failed << " failed, "
              << (passed + failed) << " total." << std::endl;
    std::cout << "========================================" << std::endl;

    return failed > 0 ? 1 : 0;
}
