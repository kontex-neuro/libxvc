#pragma once

#include <cpr/cpr.h>
#include "types.h"

namespace xvc::update::detail {
inline Result<void> configure_cloud_tls(cpr::Session &session)
{
    session.SetVerifySsl(cpr::VerifySsl{true});
#ifdef _WIN32
    // Conan CPR uses OpenSSL; its default CA bundle is not installed with the
    // portable desktop. Use Windows roots while keeping peer/hostname checks.
    const auto result = curl_easy_setopt(session.GetCurlHolder()->handle,
        CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
    if (result != CURLE_OK)
        return std::unexpected(Error{ErrorCode::NetworkUnreachable,
            std::string{"Cannot enable Windows certificate trust: "} + curl_easy_strerror(result)});
#endif
    return {};
}
} // namespace xvc::update::detail
