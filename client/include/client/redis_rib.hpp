/**
 * @file client/include/client/redis_rib.hpp
 * @brief Redis-backed RIB prefix table for the Switch Route Application.
 *
 * Provides a thin C++ wrapper around the hiredis synchronous API for
 * storing and querying IPv4 prefix → nexthop-group-ID mappings in Redis.
 *
 * @version 1.0
 */

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

struct redisContext;

namespace sra
{

/**
 * @brief Redis Hash API for the RIB prefix table.
 *
 * Stores prefix → nexthop-group-ID mappings in the Redis hash
 * @c "rib:prefix" (HSET / HGET / HDEL / HGETALL).  All operations use
 * the synchronous hiredis blocking API.
 *
 * Connection errors are handled silently: methods return a safe sentinel
 * (false / nullopt / empty map) when the context is null.
 *
 * @note The key name is available as the public constant @c kHashKey.
 */
class RedisRib
{
public:
    static constexpr const char* kHashKey = "rib:prefix"; ///< Redis hash key for all RIB prefix entries.

    /**
     * @brief Connects to the Redis server at @p host : @p port.
     *
     * On failure the internal context is set to @c nullptr and an error
     * message is written to @c stderr.  Callers should check @c connected()
     * before issuing any commands.
     *
     * @param host  Redis server hostname or IP address (default: "127.0.0.1").
     * @param port  Redis server port (default: 6379).
     */
    explicit RedisRib(const std::string& host = "127.0.0.1", int port = 6379);

    /** @brief Destructor — frees the hiredis context if connected. */
    ~RedisRib();

    RedisRib(const RedisRib&) = delete;
    RedisRib& operator=(const RedisRib&) = delete;

    /**
     * @brief Stores or updates a prefix → nexthop-group-ID mapping (HSET).
     *
     * @param prefix  IPv4 prefix string, e.g. @c "10.1.0.0/24".
     * @param nhg_id  Nexthop-group ID string, e.g. @c "100".
     * @return @c true on success; @c false when disconnected or on Redis error.
     */
    bool set(const std::string& prefix, const std::string& nhg_id);

    /**
     * @brief Looks up the nexthop-group ID for @p prefix (HGET).
     *
     * @param prefix  IPv4 prefix string to look up.
     * @return The nexthop-group ID string, or @c std::nullopt when the field
     *         is absent or the connection is unavailable.
     */
    std::optional<std::string> get(const std::string& prefix) const;

    /**
     * @brief Removes the entry for @p prefix from the hash (HDEL).
     *
     * @param prefix  IPv4 prefix string to remove.
     * @return @c true when the field existed and was deleted; @c false
     *         otherwise.
     */
    bool del(const std::string& prefix);

    /**
     * @brief Deletes the entire @c "rib:prefix" hash (DEL).
     *
     * @return @c true on success; @c false when disconnected or on Redis error.
     */
    bool clear();

    /**
     * @brief Returns all prefix → nexthop-group-ID entries (HGETALL).
     *
     * @return An unordered map of all entries, or an empty map when
     *         disconnected or on Redis error.
     */
    std::unordered_map<std::string, std::string> getAll() const;

    /** @brief Returns @c true when the connection is active. */
    bool connected() const noexcept
    {
        return ctx_ != nullptr;
    }

private:
    redisContext* ctx_ = nullptr;
};

} // namespace sra
