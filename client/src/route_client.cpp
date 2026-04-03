/**
 * @file client/src/route_client.cpp
 * @brief gRPC client implementation for the Switch Route Application.
 *
 * @version 1.0
 */

#include "client/route_client.hpp"

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <format>
#include <fstream>
#include <memory>

namespace sra
{

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

RouteClient::RouteClient(std::string serverAddress,
                         bool useTls,
                         std::string caCert,
                         int timeoutSeconds,
                         std::string login,
                         std::string password)
    : serverAddress_(std::move(serverAddress)), timeoutSeconds_(timeoutSeconds),
      login_(std::move(login)), password_(std::move(password))
{
    if (useTls)
    {
        grpc::SslCredentialsOptions sslOpts;
        if (!caCert.empty())
        {
            std::ifstream caFile(caCert);
            sslOpts.pem_root_certs =
                std::string(std::istreambuf_iterator<char>(caFile), {});
        }
        channel_ =
            grpc::CreateChannel(serverAddress_, grpc::SslCredentials(sslOpts));
    }
    else
    {
        channel_ = grpc::CreateChannel(serverAddress_,
                                       grpc::InsecureChannelCredentials());
    }
    stub_ = srmd::v1::SwitchRouteManager::NewStub(channel_);
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::unique_ptr<grpc::ClientContext> RouteClient::makeContext() const
{
    auto ctx = std::make_unique<grpc::ClientContext>();
    ctx->set_deadline(std::chrono::system_clock::now() +
                      std::chrono::seconds(timeoutSeconds_));
    if (!login_.empty())
    {
        ctx->AddMetadata("x-login", login_);
    }
    if (!password_.empty())
    {
        ctx->AddMetadata("x-password", password_);
    }
    return ctx;
}

std::string RouteClient::statusToError(const grpc::Status& status)
{
    return std::format("gRPC error {}: {}",
                       static_cast<int>(status.error_code()),
                       status.error_message());
}

// ---------------------------------------------------------------------------
// Echo
// ---------------------------------------------------------------------------

std::expected<srmd::v1::EchoResponse, std::string>
RouteClient::echo(const std::string& message)
{
    srmd::v1::EchoRequest req;
    req.set_message(message);
    req.set_client_ts_us(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    srmd::v1::EchoResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->Echo(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    return resp;
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

std::expected<srmd::v1::HeartbeatResponse, std::string>
RouteClient::heartbeat(uint64_t sequence)
{
    srmd::v1::HeartbeatRequest req;
    req.set_sequence(sequence);
    req.set_client_ts_us(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    srmd::v1::HeartbeatResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->Heartbeat(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    return resp;
}

// ---------------------------------------------------------------------------
// AddRoute
// ---------------------------------------------------------------------------

std::expected<srmd::v1::Route, std::string>
RouteClient::addRoute(const std::string& destination,
                      const std::string& gateway,
                      const std::string& interfaceName,
                      uint32_t metric,
                      srmd::v1::AddressFamily family,
                      srmd::v1::RouteProtocol protocol)
{
    srmd::v1::AddRouteRequest req;
    req.set_destination(destination);
    req.set_gateway(gateway);
    req.set_interface_name(interfaceName);
    req.set_metric(metric);
    req.set_address_family(family);
    req.set_protocol(protocol);

    srmd::v1::RouteResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->AddRoute(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    if (resp.code() != srmd::v1::STATUS_CODE_OK)
    {
        return std::unexpected(std::format(
            "AddRoute failed ({}): {}",
            srmd::v1::StatusCode_Name(resp.code()), resp.message()));
    }
    return resp.route();
}

// ---------------------------------------------------------------------------
// RemoveRoute
// ---------------------------------------------------------------------------

std::expected<void, std::string> RouteClient::removeRoute(const std::string& id)
{
    srmd::v1::RemoveRouteRequest req;
    req.set_id(id);

    srmd::v1::StatusResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->RemoveRoute(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    if (resp.code() != srmd::v1::STATUS_CODE_OK)
    {
        return std::unexpected(std::format(
            "RemoveRoute failed ({}): {}",
            srmd::v1::StatusCode_Name(resp.code()), resp.message()));
    }
    return {};
}

// ---------------------------------------------------------------------------
// GetRoute
// ---------------------------------------------------------------------------

std::expected<srmd::v1::Route, std::string>
RouteClient::getRoute(const std::string& id)
{
    srmd::v1::GetRouteRequest req;
    req.set_id(id);

    srmd::v1::RouteResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->GetRoute(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    if (resp.code() != srmd::v1::STATUS_CODE_OK)
    {
        return std::unexpected(std::format(
            "GetRoute failed ({}): {}",
            srmd::v1::StatusCode_Name(resp.code()), resp.message()));
    }
    return resp.route();
}

// ---------------------------------------------------------------------------
// ListRoutes
// ---------------------------------------------------------------------------

std::expected<std::vector<srmd::v1::Route>, std::string>
RouteClient::listRoutes(bool activeOnly)
{
    srmd::v1::ListRoutesRequest req;
    req.set_active_only(activeOnly);

    srmd::v1::ListRoutesResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->ListRoutes(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    if (resp.code() != srmd::v1::STATUS_CODE_OK)
    {
        return std::unexpected(
            std::format("ListRoutes failed ({})",
                        srmd::v1::StatusCode_Name(resp.code())));
    }
    return std::vector<srmd::v1::Route>(resp.routes().begin(),
                                        resp.routes().end());
}

// ---------------------------------------------------------------------------
// SetLoopback
// ---------------------------------------------------------------------------

std::expected<std::string, std::string>
RouteClient::setLoopback(const std::string& address)
{
    srmd::v1::SetLoopbackRequest req;
    req.set_address(address);

    srmd::v1::SetLoopbackResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->SetLoopback(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    if (resp.code() != srmd::v1::STATUS_CODE_OK)
    {
        return std::unexpected(std::format(
            "SetLoopback failed ({}): {}",
            srmd::v1::StatusCode_Name(resp.code()), resp.message()));
    }
    return resp.address();
}

// ---------------------------------------------------------------------------
// GetLoopback
// ---------------------------------------------------------------------------

std::expected<std::string, std::string> RouteClient::getLoopback()
{
    srmd::v1::GetLoopbackRequest req;

    srmd::v1::GetLoopbackResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->GetLoopback(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    if (resp.code() != srmd::v1::STATUS_CODE_OK)
    {
        return std::unexpected(std::format(
            "GetLoopback failed ({}): {}",
            srmd::v1::StatusCode_Name(resp.code()), resp.message()));
    }
    return resp.address();
}

// ---------------------------------------------------------------------------
// GetLoopbacks
// ---------------------------------------------------------------------------

std::expected<srmd::v1::GetLoopbacksResponse, std::string>
RouteClient::getLoopbacks(const std::string& loopback)
{
    srmd::v1::GetLoopbacksRequest req;
    req.set_loopback(loopback);

    srmd::v1::GetLoopbacksResponse resp;
    auto ctx = makeContext();
    const grpc::Status status = stub_->GetLoopbacks(ctx.get(), req, &resp);
    if (!status.ok())
    {
        return std::unexpected(statusToError(status));
    }
    if (resp.code() != srmd::v1::STATUS_CODE_OK)
    {
        return std::unexpected(std::format(
            "GetLoopbacks failed ({}): {}",
            srmd::v1::StatusCode_Name(resp.code()), resp.message()));
    }
    return resp;
}

} // namespace sra
