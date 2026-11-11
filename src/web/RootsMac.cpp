// src/web/RootsMac.cpp: the system's roots on macOS. libcurl's own Keychain support needs OpenSSL or GnuTLS, and its
// TLS here is Mbed TLS, so the roots are read from the Keychain once, the first time a request needs them, as PEM,
// and handed to each request as a blob (CURLOPT_CAINFO_BLOB). Mbed TLS takes every certificate in it as a root for
// TLS to any host, so only what the Keychain says is one goes in, as Security.framework would judge it for TLS:
// - the system's anchors (SecTrustCopyAnchorCertificates, Apple's built-in roots);
// - a certificate of the system domain's or the admin domain's trust settings (roots an administrator or a profile
//   installed) whose settings make it a root for TLS: an empty list of settings, or one whose first setting that
//   applies to TLS (no policy, or the SSL policy; no application, no host name, any key usage) says TrustRoot or
//   TrustAsRoot (or no result, which is TrustRoot);
// - and none that either domain denies for TLS (a Deny in any setting with no policy or the SSL policy, whatever else
//   it restricts), an anchor of Apple's included: an administrator's distrust of a built-in root holds.
// A certificate trusted only for another policy (S/MIME, code signing), for one host name or one application is left
// out. The user's own domain is not read: a root a user (or a program running as one) adds is not one this library
// trusts. What trustd adds on top of the settings (Apple's partial or date-based distrust of a root) is not applied.
#if defined(__APPLE__)

#include "web/Roots.h"

#include <CoreFoundation/CoreFoundation.h>
#include <curl/curl.h>
#include <Security/Security.h>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace fminus::web
{
    namespace
    {
        // A certificate's DER bytes, or nothing.
        std::string derOf(SecCertificateRef certificate)
        {
            CFDataRef der = SecCertificateCopyData(certificate);
            if (der == nullptr)
                return {};
            std::string bytes(reinterpret_cast<const char*>(CFDataGetBytePtr(der)),
                              static_cast<std::size_t>(CFDataGetLength(der)));
            CFRelease(der);
            return bytes;
        }

        void appendPem(std::string& pem, const std::string& der)
        {
            static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            const auto* bytes = reinterpret_cast<const unsigned char*>(der.data());
            const std::size_t length = der.size();
            pem += "-----BEGIN CERTIFICATE-----\n";
            int column = 0;
            for (std::size_t i = 0; i < length; i += 3)
            {
                const unsigned b0 = bytes[i];
                const unsigned b1 = i + 1 < length ? bytes[i + 1] : 0;
                const unsigned b2 = i + 2 < length ? bytes[i + 2] : 0;
                const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
                pem += kAlphabet[(triple >> 18) & 63];
                pem += kAlphabet[(triple >> 12) & 63];
                pem += i + 1 < length ? kAlphabet[(triple >> 6) & 63] : '=';
                pem += i + 2 < length ? kAlphabet[triple & 63] : '=';
                column += 4;
                if (column == 64)
                {
                    pem += '\n';
                    column = 0;
                }
            }
            if (column != 0)
                pem += '\n';
            pem += "-----END CERTIFICATE-----\n";
        }

        SecCertificateRef certificateAt(CFArrayRef certificates, CFIndex i)
        {
            return static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(certificates, i)));
        }

        enum class Trust
        {
            Unspecified,
            Root,
            Deny,
        };

        // Whether a setting is for TLS: no policy, or Apple's SSL policy.
        bool forTls(CFDictionaryRef setting)
        {
            const void* policy = CFDictionaryGetValue(setting, kSecTrustSettingsPolicy);
            if (policy == nullptr)
                return true;
            if (CFGetTypeID(policy) != SecPolicyGetTypeID())
                return false;
            CFDictionaryRef properties = SecPolicyCopyProperties(static_cast<SecPolicyRef>(const_cast<void*>(policy)));
            if (properties == nullptr)
                return false;
            const void* oid = CFDictionaryGetValue(properties, kSecPolicyOid);
            const bool ssl = oid != nullptr && CFEqual(oid, kSecPolicyAppleSSL);
            CFRelease(properties);
            return ssl;
        }

        // What `domain`'s trust settings make of `certificate` for TLS to any host (the file's comment).
        Trust trustOf(SecCertificateRef certificate, SecTrustSettingsDomain domain)
        {
            CFArrayRef settings = nullptr;
            if (SecTrustSettingsCopyTrustSettings(certificate, domain, &settings) != errSecSuccess
                || settings == nullptr)
                return Trust::Unspecified;
            const CFIndex count = CFArrayGetCount(settings);
            Trust trust = count == 0 ? Trust::Root : Trust::Unspecified;   // no settings: "always trust" as a root
            bool decided = count == 0;
            for (CFIndex i = 0; i < count; ++i)
            {
                const void* value = CFArrayGetValueAtIndex(settings, i);
                if (value == nullptr || CFGetTypeID(value) != CFDictionaryGetTypeID())
                    continue;
                const auto setting = static_cast<CFDictionaryRef>(value);
                if (!forTls(setting))
                    continue;
                SInt32 number = static_cast<SInt32>(kSecTrustSettingsResultTrustRoot);   // a setting without a result
                if (const void* given = CFDictionaryGetValue(setting, kSecTrustSettingsResult);
                    given != nullptr && CFGetTypeID(given) == CFNumberGetTypeID())
                    CFNumberGetValue(static_cast<CFNumberRef>(given), kCFNumberSInt32Type, &number);
                const auto result = static_cast<std::uint32_t>(number);
                if (result == kSecTrustSettingsResultDeny)
                {
                    trust = Trust::Deny;   // whatever it is restricted to, and whatever came before
                    break;
                }
                if (decided)
                    continue;
                bool restricted = CFDictionaryContainsKey(setting, kSecTrustSettingsApplication)
                                  || CFDictionaryContainsKey(setting, kSecTrustSettingsPolicyString);
                if (const void* usage = CFDictionaryGetValue(setting, kSecTrustSettingsKeyUsage); usage != nullptr)
                {
                    SInt32 bits = 0;
                    restricted = restricted || CFGetTypeID(usage) != CFNumberGetTypeID()
                                 || !CFNumberGetValue(static_cast<CFNumberRef>(usage), kCFNumberSInt32Type, &bits)
                                 || static_cast<std::uint32_t>(bits) != kSecTrustSettingsKeyUseAny;
                }
                if (restricted)
                    continue;
                if (result == kSecTrustSettingsResultTrustRoot || result == kSecTrustSettingsResultTrustAsRoot)
                {
                    trust = Trust::Root;
                    decided = true;
                }
                // kSecTrustSettingsResultUnspecified: the next setting decides.
            }
            CFRelease(settings);
            return trust;
        }

        std::string load()
        {
            std::vector<std::string> roots;
            std::set<std::string> denied;
            CFArrayRef anchors = nullptr;
            if (SecTrustCopyAnchorCertificates(&anchors) == errSecSuccess && anchors != nullptr)
            {
                for (CFIndex i = 0; i < CFArrayGetCount(anchors); ++i)
                    roots.push_back(derOf(certificateAt(anchors, i)));
                CFRelease(anchors);
            }
            for (SecTrustSettingsDomain domain : {kSecTrustSettingsDomainSystem, kSecTrustSettingsDomainAdmin})
            {
                CFArrayRef certificates = nullptr;
                if (SecTrustSettingsCopyCertificates(domain, &certificates) != errSecSuccess || certificates == nullptr)
                    continue;
                for (CFIndex i = 0; i < CFArrayGetCount(certificates); ++i)
                {
                    SecCertificateRef certificate = certificateAt(certificates, i);
                    switch (trustOf(certificate, domain))
                    {
                    case Trust::Root:
                        roots.push_back(derOf(certificate));
                        break;
                    case Trust::Deny:
                        denied.insert(derOf(certificate));
                        break;
                    case Trust::Unspecified:
                        break;
                    }
                }
                CFRelease(certificates);
            }
            std::string pem;
            std::set<std::string> added;
            for (const std::string& der : roots)
            {
                if (!der.empty() && !denied.contains(der) && added.insert(der).second)
                    appendPem(pem, der);
            }
            return pem;
        }
    } // namespace

    bool useSystemRoots(CURL* easy)
    {
        static const std::string pem = load();   // on the I/O thread, the first time
        if (pem.empty())
            return false;
        curl_blob blob{};
        blob.data = const_cast<char*>(pem.data());
        blob.len = pem.size();
        blob.flags = CURL_BLOB_NOCOPY;   // the static string outlives every request
        return curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &blob) == CURLE_OK;
    }
} // namespace fminus::web

#endif
