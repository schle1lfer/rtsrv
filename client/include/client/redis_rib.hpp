#pragma once

#include <optional>
#include <string>
#include <unordered_map>

struct redisContext;

namespace sra {

/**
 * Redis Hash API for the RIB prefix table.
 *
 * Stores entries in the Redis hash "rib:prefix":
 *   field = IPv4 prefix string (e.g. "10.1.0.0/24")
 *   value = nexthop-group ID string (e.g. "100")
 *
 * All operations are synchronous (hiredis blocking API).
 */
class RedisRib
{
public:
    static constexpr const char* kHashKey = "rib:prefix";

    explicit RedisRib(const std::string& host = "127.0.0.1", int port = 6379);
    ~RedisRib();

    RedisRib(const RedisRib&)            = delete;
    RedisRib& operator=(const RedisRib&) = delete;

    /** HSET rib:prefix <prefix> <nhg_id>. Returns true on success. */
    bool set(const std::string& prefix, const std::string& nhg_id);

    /** HGET rib:prefix <prefix>. Returns nullopt when the field is absent. */
    std::optional<std::string> get(const std::string& prefix) const;

    /** HDEL rib:prefix <prefix>. Returns true when the field existed. */
    bool del(const std::string& prefix);

    /** DEL rib:prefix — removes the entire hash. Returns true on success. */
    bool clear();

    /** HGETALL rib:prefix → map of all prefix→nhg_id entries. */
    std::unordered_map<std::string, std::string> getAll() const;

    bool connected() const noexcept { return ctx_ != nullptr; }

private:
    redisContext* ctx_ = nullptr;
};

} // namespace sra
