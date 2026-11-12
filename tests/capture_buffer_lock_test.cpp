#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <vector>

using namespace std::chrono_literals;
using VkBuffer = uint64_t;
using VmaAllocation = void*;
constexpr unsigned VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1;
constexpr uint64_t MB = 1ull << 20;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)

struct FreeBuf { VkBuffer buffer; VmaAllocation alloc; void* map; uint64_t size; };
std::vector<FreeBuf> cap_free;
std::mutex cap_free_mu;
uint64_t cap_free_bytes = 0;
std::atomic<bool> cap_shutting_down{false};
bool env_on(const char*) { return false; }
bool cap_prewarm_on() { return false; }
void cap_prewarm_class(uint64_t) {}
uint64_t cap_spare_max() { return 32 * MB; }
std::atomic<int> destroyed{0};
void destroy_buffer(VkBuffer, VmaAllocation) { ++destroyed; }

// Stop the allocation at the driver boundary, after production code has chosen
// its cache-miss path. Other threads must still be able to return and take a spare.
std::mutex driver_mu;
std::condition_variable driver_cv;
int entered = 0;
bool release_driver = false, fail_allocation = false;
std::atomic<uint64_t> next_buffer{100};
bool create_buffer_cached(uint64_t size, unsigned usage, VkBuffer& buffer,
                          VmaAllocation& allocation, void** mapped, bool dedicated) {
    CHECK(size >= 2 * MB && usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CHECK(dedicated == (size >= 16 * MB));
    std::unique_lock lock(driver_mu);
    ++entered;
    driver_cv.notify_all();
    driver_cv.wait(lock, [] { return release_driver; });
    if (fail_allocation) return false;
    buffer = next_buffer++;
    allocation = reinterpret_cast<void*>(static_cast<uintptr_t>(buffer));
    *mapped = allocation;
    return true;
}

/* ACTUAL_FUNCTIONS */

struct Result {
    VkBuffer buffer = 0;
    VmaAllocation alloc = nullptr;
    void* map = nullptr;
    uint64_t size = 0;
    bool ok = false;
};
Result get(uint64_t need) {
    Result r;
    r.ok = cap_buf_get(need, r.buffer, r.alloc, r.map, r.size);
    return r;
}
void put(VkBuffer id, uint64_t bytes, bool worker = false) {
    auto p = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
    cap_buf_free(id, p, p, bytes, worker);
}
void release() {
    std::lock_guard lock(driver_mu);
    release_driver = true;
    driver_cv.notify_all();
}
bool wait_entered(int count) {
    std::unique_lock lock(driver_mu);
    return driver_cv.wait_for(lock, 5s, [=] { return entered >= count; });
}
void reset_driver(bool fail = false) {
    std::lock_guard lock(driver_mu);
    entered = 0;
    release_driver = false;
    fail_allocation = fail;
}
void check_best_fit_and_budget() {
    put(1, 8 * MB);
    put(2, 4 * MB);
    put(3, 2 * MB);
    auto r = get(3 * MB);
    CHECK(r.ok && r.buffer == 2 && r.size == 4 * MB);
    CHECK(cap_free_bytes == 10 * MB && cap_free.size() == 2 && entered == 0);
    put(4, 32 * MB);  // Existing spare budget still applies.
    CHECK(destroyed == 1 && cap_free_bytes == 10 * MB);
    CHECK(get(8 * MB).buffer == 1 && get(MB).buffer == 3);
    CHECK(cap_free.empty() && cap_free_bytes == 0);
}
void check_recycle_during_allocation(bool fail, bool worker) {
    reset_driver(fail);
    auto allocation = std::async(std::launch::async, [] { return get(20 * MB); });
    CHECK(wait_entered(1));
    auto recycle = std::async(std::launch::async, [=] { put(10, 8 * MB, worker); });
    const bool returned = recycle.wait_for(2s) == std::future_status::ready;
    // Release the simulated driver even on failure, so the negative control joins
    // its threads and reports an assertion instead of hanging on future destruction.
    if (!returned) {
        release();
        recycle.get();
        (void)allocation.get();
        CHECK(returned);
    }
    recycle.get();
    auto reuse = std::async(std::launch::async, [] { return get(4 * MB); });
    const bool reused = reuse.wait_for(2s) == std::future_status::ready;
    if (!reused) release();
    auto spare = reuse.get();
    release();
    auto fresh = allocation.get();
    CHECK(reused && spare.ok && spare.buffer == 10 && spare.size == 8 * MB);
    CHECK(fresh.ok == !fail && fresh.size == 32 * MB && entered == 1);
    CHECK(fail ? !fresh.map : (fresh.map && fresh.buffer != spare.buffer));
    CHECK(cap_free.empty() && cap_free_bytes == 0);
}
void check_independent_misses() {
    reset_driver();
    auto a = std::async(std::launch::async, [] { return get(MB); });
    CHECK(wait_entered(1));
    auto b = std::async(std::launch::async, [] { return get(3 * MB); });
    const bool independent = wait_entered(2);
    release();
    auto x = a.get(), y = b.get();
    CHECK(independent && x.ok && y.ok && x.buffer != y.buffer);
    CHECK(x.size == 2 * MB && y.size == 4 * MB);
    CHECK(cap_free.empty() && cap_free_bytes == 0);
}
int main() {
    // Explicit environment overrides must not change the fixture's size/budget cases.
#ifdef _WIN32
    _putenv_s("RX_VK_CAP_MIN_MB", "2");
#else
    setenv("RX_VK_CAP_MIN_MB", "2", 1);
#endif
    check_best_fit_and_budget();
    check_recycle_during_allocation(false, false);
    check_recycle_during_allocation(false, true);
    check_recycle_during_allocation(true, false);
    check_independent_misses();
    cap_shutting_down = true;
    put(99, 2 * MB, true);
    CHECK(cap_free.empty() && cap_free_bytes == 0 && destroyed == 1);
    std::puts("capture allocation: replay/worker returns, spare reuse, concurrent misses, failure and budget checks passed");
}
