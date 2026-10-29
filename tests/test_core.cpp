#include "servicewatch.hpp"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x))                                                                                  \
            throw std::runtime_error("check failed: " #x);                                         \
    } while (false)
unsigned checks = 0;
template <class F> bool throws(F f) {
    try {
        f();
        return false;
    } catch (const std::exception &) {
        return true;
    }
}
int main() {
    try {
        sw::Network network;
        sw::Health h;
        CHECK(h.state == sw::State::unknown);
        CHECK(!h.update(false, 2, 2));
        CHECK(h.state == sw::State::unknown);
        CHECK(h.update(false, 2, 2));
        CHECK(h.state == sw::State::down);
        CHECK(!h.update(true, 2, 2));
        CHECK(h.state == sw::State::down);
        CHECK(h.update(true, 2, 2));
        CHECK(h.state == sw::State::up);
        CHECK(!h.update(false, 2, 2));
        CHECK(h.state == sw::State::up);
        CHECK(!h.update(true, 2, 2));
        CHECK(h.failures == 0 && h.successes == 1);
        for (unsigned i = 0; i < 100000; ++i)
            h.update(true, 2, 2);
        CHECK(h.successes == 2);
        CHECK(throws([&] { h.update(true, 0, 1); }));
        CHECK(sw::numeric_address("127.0.0.1"));
        CHECK(sw::numeric_address("::1"));
        CHECK(!sw::numeric_address("localhost"));
        CHECK(!sw::numeric_address("127.0.0.999"));
        CHECK(!sw::numeric_address("fe80::1%eth0"));
        CHECK(!sw::numeric_address(std::string("127.0.0.1\0x", 11)));
        std::istringstream good("# comment\n web , 127.0.0.1 , 80\r\nv6,::1,65535");
        auto endpoints = sw::read_config(good);
        CHECK(endpoints.size() == 2);
        CHECK(endpoints[1].port == 65535);
        for (const auto &text : std::vector<std::string>{
                 "", "x,localhost,80", "x,127.0.0.1,0", "x,127.0.0.1,65536", "x,127.0.0.1,-1",
                 "x,127.0.0.1,1x", "x,127.0.0.1,80,more", "x,127.0.0.1,80\nx,::1,80",
                 "bad\"name,127.0.0.1,80", std::string(257, '#'), std::string(65537, '\n')}) {
            std::istringstream in(text);
            CHECK(throws([&] { sw::read_config(in); }));
        }
        CHECK(
            throws([&] { sw::tcp_probe({"x", "localhost", 80}, std::chrono::milliseconds(10)); }));
        CHECK(throws(
            [&] { sw::check_round(endpoints, 0, std::chrono::milliseconds(1), sw::tcp_probe); }));
        std::vector<sw::Endpoint> jobs;
        for (unsigned i = 0; i < 20; ++i)
            jobs.push_back({std::to_string(i), "127.0.0.1", 80});
        std::atomic<unsigned> active{0}, peak{0}, calls{0};
        std::mutex mutex;
        std::condition_variable cv;
        unsigned arrived = 0;
        const auto observations = sw::check_round(
            jobs, 4, std::chrono::milliseconds(40),
            [&](const sw::Endpoint &e, std::chrono::milliseconds timeout) {
                const unsigned current = ++active;
                unsigned previous = peak.load();
                while (current > previous && !peak.compare_exchange_weak(previous, current)) {
                }
                unsigned index = calls.fetch_add(1);
                if (index < 4) {
                    std::unique_lock<std::mutex> lock(mutex);
                    ++arrived;
                    cv.notify_all();
                    if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return arrived == 4; }))
                        throw std::runtime_error("workers did not run concurrently");
                }
                if (timeout.count() != 40)
                    throw std::runtime_error("timeout not forwarded");
                --active;
                return sw::Observation{true, std::stoll(e.name), "connected"};
            });
        CHECK(calls == 20);
        CHECK(peak == 4);
        CHECK(active == 0);
        for (std::size_t i = 0; i < jobs.size(); ++i)
            CHECK(observations[i].elapsed_ms == static_cast<std::int64_t>(i));
        std::atomic<unsigned> live{0};
        CHECK(throws([&] {
            sw::check_round(jobs, 4, std::chrono::milliseconds(20),
                            [&](const sw::Endpoint &e, std::chrono::milliseconds) {
                                struct Guard {
                                    std::atomic<unsigned> &n;
                                    Guard(std::atomic<unsigned> &count) : n(count) { ++n; }
                                    ~Guard() { --n; }
                                } guard(live);
                                if (e.name == "0")
                                    throw std::runtime_error("injected probe failure");
                                return sw::Observation{false, 20, "timeout"};
                            });
        }));
        CHECK(live == 0);
        auto again = sw::check_round(jobs, 1, std::chrono::milliseconds(20),
                                     [](const sw::Endpoint &, std::chrono::milliseconds) {
                                         return sw::Observation{false, 20, "timeout"};
                                     });
        CHECK(again.size() == 20);
        CHECK(!again[0].reachable && again[0].reason == "timeout");
        std::cout << checks
                  << " assertions passed; concurrency, ordering, exception cleanup and state "
                     "thresholds verified\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
