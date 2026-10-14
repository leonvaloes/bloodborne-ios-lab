#include "check.h"
#include "dispatcher.h"
#include "fixtures.h"

int main() { return run([] {
    using namespace bb;
    IndirectDispatcher d;
    CHECK(d.size() == 0);

    d.register_target(0x1000, fixture_add64);
    d.register_target(0x2000, fixture_sub64);
    d.register_target(0x3000, fixture_xor64);
    CHECK(d.size() == 3);

    CHECK(d.has_target(0x1000));
    CHECK(d.has_target(0x2000));
    CHECK(d.has_target(0x3000));
    CHECK(!d.has_target(0x4000));

    CHECK(d.dispatch(0x1000, 20, 30).value == 50);
    CHECK(d.dispatch(0x2000, 50, 20).value == 30);
    CHECK(d.dispatch(0x3000, 0xFF, 0x0F).value == 0xF0);

    bool threw = false;
    try {
        d.dispatch(0x9999, 1, 1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    bool null_rejected = false;
    try {
        d.register_target(0x5000, nullptr);
    } catch (const std::invalid_argument&) {
        null_rejected = true;
    }
    CHECK(null_rejected);

    d.clear();
    CHECK(d.size() == 0);
    CHECK(!d.has_target(0x1000));

    std::cout << "dispatcher: indirect target lookup, execution and trap handling passed\n";
}); }
