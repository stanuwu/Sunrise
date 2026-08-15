#include "server_runtime.h"

#include "../../client/network/consumer.h"
#include "../../core/logging/log.h"
#include "../bap/runtime.h"
#include "../http/server_http.h"
#include "../transport/bap_listener.h"
#include "../ui/runtime/server_ui_module_runtime.h"

namespace sunrise::server {

/** Registers Server consumers with the Client networking boundary. */
bool initialize() noexcept {
    if (!client::network::register_http_consumer(&http::consume)) {
        return false;
    }
    if (client::network::register_bap_consumer(&bap::consume)) {
        // HTTP and UI remain useful when the local BAP port is already owned.
        if (!transport::initialize()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=transport stage=listen result=fail");
        }
        if (ui::runtime::initialize()) {
            return true;
        }
        transport::shutdown();
        client::network::unregister_bap_consumer(&bap::consume);
    }
    // BAP registration failure rolls back the earlier HTTP registration.
    client::network::unregister_http_consumer(&http::consume);
    return false;
}

/** Runs one bounded server service slice. @param now Monotonic tick count. */
void service(std::uint64_t now) noexcept {
    transport::service(now);
}

/** Unregisters Server consumers in reverse registration order. */
void shutdown() noexcept {
    ui::runtime::shutdown();
    transport::shutdown();
    client::network::unregister_bap_consumer(&bap::consume);
    client::network::unregister_http_consumer(&http::consume);
    bap::shutdown();
}

} // namespace sunrise::server
