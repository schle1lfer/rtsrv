/**
 * @file client/include/client/switch_config.hpp
 * @brief Switch configuration loader for sra.
 *
 * Parses a JSON file that lists one or more Switch Route Manager servers
 * (switches).  Each entry describes the network coordinates and credentials
 * needed to open a gRPC channel to that server.
 *
 * Expected JSON schema (example):
 * @code
 * {
 *   "switches": [
 *     {
 *       "name":     "sw-core-01",
 *       "ipv4":     "192.168.1.10",
 *       "grpc_port": 50051,
 *       "login":    "admin",
 *       "password": "secret"
 *     }
 *   ]
 * }
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace sra
{

/**
 * @brief Describes a single switch (srmd instance) to connect to.
 *
 * Populated from one element of the @c switches array in the JSON config.
 */
struct SwitchEntry
{
    /** @brief Human-readable label (optional; defaults to @c ipv4:grpc_port).
     */
    std::string name;

    /** @brief IPv4 address of the srmd host (e.g. "192.168.1.10"). */
    std::string ipv4;

    /** @brief gRPC listen port on the remote host (e.g. 50051). */
    uint16_t grpc_port{50051};

    /** @brief Login credential sent as gRPC call metadata. */
    std::string login;

    /** @brief Password credential sent as gRPC call metadata. */
    std::string password;

    /**
     * @brief Returns the gRPC target string for this entry.
     *
     * Format: @c "<ipv4>:<grpc_port>"
     *
     * @return Target string suitable for @c grpc::CreateChannel().
     */
    [[nodiscard]] std::string target() const;

    /**
     * @brief Returns the display label for this entry.
     *
     * Returns @c name if non-empty, otherwise @c target().
     *
     * @return Human-readable identifier.
     */
    [[nodiscard]] std::string label() const;
};

/**
 * @brief Loads and parses a switch configuration JSON file.
 *
 * Reads the file at @p path and deserialises every element of the
 * top-level @c "switches" array into a @c SwitchEntry.
 *
 * @param path  Filesystem path to the JSON configuration file.
 * @return      Non-empty vector of @c SwitchEntry on success, or an error
 *              string describing why parsing failed.
 */
[[nodiscard]] std::expected<std::vector<SwitchEntry>, std::string>
loadSwitchConfig(const std::string& path);

} // namespace sra
