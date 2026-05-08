#include "client/redis_rib.hpp"

#include <hiredis/hiredis.h>

#include <cstdio>

namespace sra {

RedisRib::RedisRib(const std::string& host, int port)
{
    ctx_ = redisConnect(host.c_str(), port);
    if (!ctx_ || ctx_->err)
    {
        if (ctx_)
        {
            std::fprintf(stderr,
                         "[redis_rib] connect %s:%d failed: %s\n",
                         host.c_str(),
                         port,
                         ctx_->errstr);
            redisFree(ctx_);
        }
        else
        {
            std::fprintf(stderr,
                         "[redis_rib] connect %s:%d failed: OOM\n",
                         host.c_str(),
                         port);
        }
        ctx_ = nullptr;
    }
}

RedisRib::~RedisRib()
{
    if (ctx_)
        redisFree(ctx_);
}

bool RedisRib::clear()
{
    if (!ctx_)
        return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "DEL %s", kHashKey));
    if (!reply)
        return false;

    const bool ok = (reply->type != REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    return ok;
}

bool RedisRib::set(const std::string& prefix, const std::string& nhg_id)
{
    if (!ctx_)
        return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HSET %s %s %s", kHashKey,
                     prefix.c_str(), nhg_id.c_str()));
    if (!reply)
        return false;

    const bool ok = (reply->type != REDIS_REPLY_ERROR);
    freeReplyObject(reply);
    return ok;
}

std::optional<std::string> RedisRib::get(const std::string& prefix) const
{
    if (!ctx_)
        return std::nullopt;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HGET %s %s", kHashKey, prefix.c_str()));
    if (!reply)
        return std::nullopt;

    std::optional<std::string> result;
    if (reply->type == REDIS_REPLY_STRING)
        result = std::string(reply->str, static_cast<std::size_t>(reply->len));

    freeReplyObject(reply);
    return result;
}

bool RedisRib::del(const std::string& prefix)
{
    if (!ctx_)
        return false;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HDEL %s %s", kHashKey, prefix.c_str()));
    if (!reply)
        return false;

    const bool existed = (reply->type == REDIS_REPLY_INTEGER && reply->integer > 0);
    freeReplyObject(reply);
    return existed;
}

std::unordered_map<std::string, std::string> RedisRib::getAll() const
{
    std::unordered_map<std::string, std::string> result;
    if (!ctx_)
        return result;

    auto* reply = static_cast<redisReply*>(
        redisCommand(ctx_, "HGETALL %s", kHashKey));
    if (!reply)
        return result;

    if (reply->type == REDIS_REPLY_ARRAY && reply->elements % 2 == 0)
    {
        for (std::size_t i = 0; i < reply->elements; i += 2)
        {
            const redisReply* k = reply->element[i];
            const redisReply* v = reply->element[i + 1];
            if (k->type == REDIS_REPLY_STRING && v->type == REDIS_REPLY_STRING)
                result.emplace(
                    std::string(k->str, static_cast<std::size_t>(k->len)),
                    std::string(v->str, static_cast<std::size_t>(v->len)));
        }
    }

    freeReplyObject(reply);
    return result;
}

} // namespace sra
