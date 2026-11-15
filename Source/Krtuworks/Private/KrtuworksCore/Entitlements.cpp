#include "Entitlements.h"

#include <algorithm>
#include <set>
#include <utility>

namespace krtu::detail
{
Entitlements::Entitlements(const Resolved& opts, Api& api, KeyValueStore& store, PlatformOwnership* platform, std::function<std::optional<std::string>()> subject)
    : opts_(opts), api_(api), store_(store), platform_(platform), subject_(std::move(subject))
{
}

std::string Entitlements::key(const std::string& owner) const
{
    return "krtu:" + opts_.clientId + ":entitlements:" + owner;
}

std::vector<Entitlement> Entitlements::itemsFrom(const Json& list)
{
    std::vector<Entitlement> items;
    if (!list.isArray()) return items;
    for (const auto& raw : list.items())
    {
        const auto sku = raw.getString("sku");
        const auto source = raw.getString("source");
        if (!raw.isObject() || !sku || !source) continue;
        Entitlement item;
        item.sku = *sku;
        item.source = *source;
        const auto status = raw.getString("status").value_or("");
        if (status == "ACTIVE" || status == "REVOKED") item.status = status;
        item.platformProductId = raw.getString("platformProductId").value_or("");
        item.checked = raw.getBool("checked") == std::optional<bool>(true);
        items.push_back(std::move(item));
    }
    return items;
}

void Entitlements::ensureLoaded()
{
    const auto who = subject_();
    if (!who) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (owner_ == who) return;
    }
    std::vector<Entitlement> kept;
    if (const auto stored = getJson(store_, key(*who)))
    {
        kept = itemsFrom(*stored);
        for (auto& item : kept) item.checked = false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (subject_() != who) return;
    owner_ = who;
    stored_ = std::move(kept);
    confirmed_.clear();
}

void Entitlements::forget()
{
    std::lock_guard<std::mutex> lock(mutex_);
    owner_.reset();
    stored_.clear();
    confirmed_.clear();
}

std::vector<Entitlement> Entitlements::listLocked() const
{
    std::vector<Entitlement> out;
    if (!owner_ || owner_ != subject_()) return out;
    for (const auto& item : stored_)
    {
        const auto sure = std::find_if(confirmed_.begin(), confirmed_.end(), [&](const Entitlement& c) { return c.sku == item.sku; });
        out.push_back(sure != confirmed_.end() ? *sure : item);
    }
    return out;
}

std::vector<Entitlement> Entitlements::list()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return listLocked();
}

void Entitlements::keep(const std::string& owner, std::vector<Entitlement> list)
{
    auto kept = Json::array();
    for (auto& item : list)
    {
        item.checked = false;
        auto entry = Json::object({{"sku", item.sku}, {"source", item.source}});
        if (!item.status.empty()) entry["status"] = item.status;
        if (!item.platformProductId.empty()) entry["platformProductId"] = item.platformProductId;
        kept.push(entry);
    }
    putJson(store_, key(owner), kept);
    stored_ = std::move(list);
}

void Entitlements::fromSession(const Json& items)
{
    const auto who = subject_();
    auto list = itemsFrom(items);
    if (!who || list.empty()) return;
    ensureLoaded();
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != who) return;
    confirmed_.clear();
    for (const auto& item : list)
    {
        if (!item.checked) continue;
        confirmed_.erase(std::remove_if(confirmed_.begin(), confirmed_.end(), [&](const Entitlement& c) { return c.sku == item.sku; }), confirmed_.end());
        confirmed_.push_back(item);
    }
    keep(*who, std::move(list));
}

std::vector<Entitlement> Entitlements::refresh()
{
    const auto who = subject_();
    if (!who) return {};
    ensureLoaded();
    const auto answer = api_.player("/v1/me/entitlements", PlayerCall{});
    auto items = itemsFrom(answer.body()["entitlements"]);
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != who || subject_() != who) return {};
    std::set<std::string> listed;
    for (const auto& item : items) listed.insert(item.sku);
    for (const auto& old : stored_)
    {
        if (listed.count(old.sku) != 0) continue;
        Entitlement gone = old;
        gone.status.clear();
        items.push_back(std::move(gone));
    }
    keep(*who, std::move(items));
    return listLocked();
}

bool Entitlements::ownsDlc(const std::string& dlc)
{
    std::optional<std::string> productId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!owner_ || owner_ != subject_()) return false;
        const auto sure = std::find_if(confirmed_.begin(), confirmed_.end(), [&](const Entitlement& c) { return c.sku == dlc; });
        if (sure != confirmed_.end()) return sure->status == "ACTIVE";
        const auto kept = std::find_if(stored_.begin(), stored_.end(), [&](const Entitlement& s) { return s.sku == dlc; });
        if (kept == stored_.end()) return false;
        if (platform_ == nullptr || kept->platformProductId.empty()) return kept->status == "ACTIVE";
        productId = kept->platformProductId;
        // The platform client is asked outside the lock; its answer stands in for the server's last word.
    }
    if (const auto said = platform_->ownsProduct(*productId)) return *said;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto kept = std::find_if(stored_.begin(), stored_.end(), [&](const Entitlement& s) { return s.sku == dlc; });
    return kept != stored_.end() && kept->status == "ACTIVE";
}
}  // namespace krtu::detail
