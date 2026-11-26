#include "KrtuworksCore/Error.h"

#include <utility>

namespace krtu
{
Error::Error(std::string code, std::string message, int status, std::optional<double> retryAfter, Json details)
    : code_(std::move(code)),
      message_(std::move(message)),
      status_(status),
      retryAfter_(retryAfter),
      details_(details.isObject() ? std::move(details) : Json::object()),
      what_(code_ + ": " + message_)
{
}

bool Error::isWaitingCode(const std::string& code) noexcept
{
    return code == "app_suspended" || code == "account_merged" || code == "account_merging" || code == "sanctioned" || code == "account_deletion_pending" || code == "switched_off" ||
           code == "module_disabled";
}

void CancelToken::throwIfCancelled() const
{
    if (cancelled()) throw Cancelled();
}
}  // namespace krtu
