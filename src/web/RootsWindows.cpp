// src/web/RootsWindows.cpp: the system's roots on Windows. Schannel, libcurl's TLS there, verifies against the Windows
// certificate store itself, so there is nothing to load. Revocation is checked as far as it can be (an offline
// revocation server does not fail the request): Schannel's strict check fails every request on a network that blocks
// those servers.
#if defined(_WIN32)

#include "web/Roots.h"

#include <curl/curl.h>

namespace fminus::web
{
    bool useSystemRoots(CURL* easy)
    {
        return curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT)) ==
               CURLE_OK;
    }
} // namespace fminus::web

#endif
