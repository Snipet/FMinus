// src/web/RootsLinux.cpp: the system's roots on Linux and every other system but Windows and macOS: the first of the
// usual bundle paths that can be read, probed once, the first time a request needs it.
#if !defined(_WIN32) && !defined(__APPLE__)

#include "web/Roots.h"

#include <array>
#include <cstdio>
#include <curl/curl.h>
#include <string>

namespace fminus::web
{
    namespace
    {
        // Debian and Ubuntu, Fedora and RHEL (two), openSUSE, Alpine and others, FreeBSD, and OpenBSD's.
        constexpr std::array kBundles{
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/ssl/cert.pem",
            "/usr/local/share/certs/ca-root-nss.crt",
            "/usr/local/etc/ssl/cert.pem",
        };

        std::string probe()
        {
            for (const char* path : kBundles)
            {
                if (std::FILE* file = std::fopen(path, "rb"))
                {
                    std::fclose(file);
                    return path;
                }
            }
            return {};
        }
    } // namespace

    bool useSystemRoots(CURL* easy)
    {
        static const std::string bundle = probe();   // on the I/O thread, the first time
        if (bundle.empty())
            return false;
        return curl_easy_setopt(easy, CURLOPT_CAINFO, bundle.c_str()) == CURLE_OK;
    }
} // namespace fminus::web

#endif
