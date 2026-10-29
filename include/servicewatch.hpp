#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <istream>
#include <string>
#include <vector>
namespace sw {
struct Endpoint {
    std::string name, address;
    std::uint16_t port;
};
struct Observation {
    bool reachable;
    std::int64_t elapsed_ms;
    std::string reason;
};
enum class State { unknown, up, down };
const char *label(State state);
struct Health {
    State state = State::unknown;
    unsigned successes = 0, failures = 0;
    bool update(bool reachable, unsigned fail_threshold, unsigned recovery_threshold);
};
std::vector<Endpoint> read_config(std::istream &input);
bool numeric_address(const std::string &text);
using Probe = std::function<Observation(const Endpoint &, std::chrono::milliseconds)>;
std::vector<Observation> check_round(const std::vector<Endpoint> &endpoints, unsigned workers,
                                     std::chrono::milliseconds timeout, const Probe &probe);
Observation tcp_probe(const Endpoint &endpoint, std::chrono::milliseconds timeout);
class Network {
  public:
    Network();
    ~Network();
    Network(const Network &) = delete;
    Network &operator=(const Network &) = delete;
};
} // namespace sw
