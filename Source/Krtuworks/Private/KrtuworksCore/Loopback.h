#pragma once

#include <cstdint>
#include <string>

#include "KrtuworksCore/Error.h"

namespace krtu::detail
{
/// The one request the system browser sends back to the app (RFC 8252 §7.3): a listener on 127.0.0.1 at a
/// port the system picks, answering a small page and handing the address over. Other requests (a favicon)
/// get a 404 and the listener keeps waiting. Closing it (the destructor) refuses later connections.
class LoopbackReceiver
{
public:
    LoopbackReceiver();
    ~LoopbackReceiver();
    LoopbackReceiver(const LoopbackReceiver&) = delete;
    LoopbackReceiver& operator=(const LoopbackReceiver&) = delete;

    int port() const noexcept { return port_; }
    /// The address the browser came back to; throws Cancelled once `cancel` is set.
    std::string wait(const CancelToken& cancel);

private:
    std::intptr_t socket_;
    int port_ = 0;
};
}  // namespace krtu::detail
