#include <doctest/doctest.h>
#include "ObjectHandles.h"
#include <unordered_set>

using TestHandle = mythril::InternalObjectHandle<struct TestTag>;
using TestPool = mythril::HandlePool<TestHandle, int>;

TEST_SUITE("HandlePool") {

TEST_CASE("default handle is empty") {
    TestHandle h;
    CHECK(h.empty());
    CHECK_FALSE(h.valid());
    CHECK(h.gen() == 0);
    CHECK(h.index() == 0);

    TestHandle h2;
    CHECK(h == h2);
}

TEST_CASE("create returns valid handle and get retrieves object") {
    TestPool pool;
    auto h = pool.create(42);

    CHECK(h.valid());
    CHECK_FALSE(h.empty());
    CHECK(h.gen() == 1);

    auto* ptr = pool.get(h);
    REQUIRE(ptr != nullptr);
    CHECK(*ptr == 42);
    CHECK(pool.numObjects() == 1);
}

TEST_CASE("destroy invalidates handle") {
    TestPool pool;
    auto h = pool.create(99);
    pool.destroy(h);

    CHECK(pool.get(h) == nullptr);
    CHECK(pool.numObjects() == 0);
}

TEST_CASE("free-list recycles slots with bumped generation") {
    TestPool pool;
    auto h1 = pool.create(10);
    uint32_t originalIndex = h1.index();
    uint32_t originalGen = h1.gen();
    pool.destroy(h1);

    auto h2 = pool.create(20);
    CHECK(h2.index() == originalIndex);
    CHECK(h2.gen() == originalGen + 1);
    CHECK(*pool.get(h2) == 20);
    CHECK(pool.get(h1) == nullptr);
}

TEST_CASE("handle hashing works in unordered containers") {
    TestPool pool;
    auto h1 = pool.create(1);
    auto h2 = pool.create(2);

    std::unordered_set<TestHandle> set;
    set.insert(h1);
    set.insert(h2);
    CHECK(set.size() == 2);
    CHECK(set.count(h1) == 1);
    CHECK(set.count(h2) == 1);
}

}
