// src/web/Main.cpp: an executable's web context (Web.h, fminus_web_main_start): its policy from the environment of the
// person who runs it, since an executable has no host. Nothing in the program can change it: a program has no access
// to its environment.
#include "web/Web.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // The executable's one context, whose handle its program's global holds: fminus_web_main_stop destroys it.
    fminus::web::Context* executableContext = nullptr;

    // The words of the environment's variable `name`, separated by spaces, commas or newlines (none when it is unset).
    std::vector<std::string> words(const char* name)
    {
        std::vector<std::string> out;
        const char* text = std::getenv(name);
        if (text == nullptr)
            return out;
        std::string word;
        for (const char* c = text;; ++c)
        {
            if (*c == '\0' || *c == ' ' || *c == ',' || *c == '\n' || *c == '\t' || *c == '\r')
            {
                if (!word.empty())
                    out.push_back(word);
                word.clear();
                if (*c == '\0')
                    break;
            }
            else
                word += *c;
        }
        return out;
    }

    // The variable a policy's mistake is about, as checkPolicy names its field ("server.token has ...").
    const char* variableOf([[maybe_unused]] std::string_view error)
    {
#if FMINUS_HAS_WEB_SERVER
        if (error.starts_with("server.token"))
            return "FMINUS_WEB_TOKEN";
        if (error.starts_with("server.hosts"))
            return "FMINUS_WEB_HOSTS";
        if (error.starts_with("server.files"))
            return "FMINUS_WEB_ROOT";
        if (error.starts_with("server."))
            return "FMINUS_WEB_LISTEN";
#endif
        return "FMINUS_WEB_ALLOW";
    }
} // namespace

extern "C"
{
    // In a build with the server, the server's part comes from FMINUS_WEB_LISTEN (where it may listen: unset or empty,
    // nowhere) and, with a listen only, FMINUS_WEB_TOKEN, FMINUS_WEB_HOSTS (names separated as FMINUS_WEB_ALLOW's
    // patterns are), FMINUS_WEB_ROOT (a directory whose files it serves, read now: readServerRoot, each file it skips
    // said on stderr) and FMINUS_WEB_OPEN ("1": a listen that is not loopback with no token is meant; "0" or empty: it
    // is not). A context is made when either FMINUS_WEB_ALLOW or FMINUS_WEB_LISTEN says something.
    std::int32_t fminus_web_main_start(void** context, const char* program) noexcept
    {
        *context = nullptr;
        const char* name = program != nullptr && *program != '\0' ? program : "fminus";
        try
        {
            fminus::web::Policy policy;
            policy.allow = words("FMINUS_WEB_ALLOW");
#if FMINUS_HAS_WEB_SERVER
            if (const char* listen = std::getenv("FMINUS_WEB_LISTEN"); listen != nullptr && *listen != '\0')
            {
                fminus::web::ServerPolicy& server = policy.server;
                server.listen = listen;
                if (const char* token = std::getenv("FMINUS_WEB_TOKEN"); token != nullptr)
                    server.token = token;
                server.hosts = words("FMINUS_WEB_HOSTS");
                if (const char* open = std::getenv("FMINUS_WEB_OPEN"); open != nullptr)
                {
                    const std::string_view value(open);
                    if (value != "1" && value != "0" && !value.empty())
                    {
                        std::fprintf(stderr, "%s: FMINUS_WEB_OPEN: '%s' is not 1 or 0\n", name, open);
                        return 2;
                    }
                    server.open = value == "1";
                }
                if (const char* root = std::getenv("FMINUS_WEB_ROOT"); root != nullptr && *root != '\0')
                {
                    std::vector<std::string> skipped;
                    if (const std::string wrong = fminus::web::readServerRoot(root, server.files, skipped);
                        !wrong.empty())
                    {
                        std::fprintf(stderr, "%s: FMINUS_WEB_ROOT: %s\n", name, wrong.c_str());
                        return 2;
                    }
                    for (const std::string& note : skipped)
                        std::fprintf(stderr, "%s: FMINUS_WEB_ROOT: skipped %s\n", name, note.c_str());
                }
            }
            if (policy.allow.empty() && policy.server.listen.empty())
                return 0;   // the default: no network, no server
#else
            if (policy.allow.empty())
                return 0;   // the default: no network
#endif
            if (const char* caFile = std::getenv("FMINUS_WEB_CA_FILE"); caFile != nullptr)
                policy.caFile = caFile;
            std::string error;
            std::unique_ptr<fminus::web::Context> made = fminus::web::Context::create(policy, error);
            if (made == nullptr)
            {
                std::fprintf(stderr, "%s: %s: %s\n", name, variableOf(error), error.c_str());
                return 2;
            }
            executableContext = made.release();
            *context = executableContext->handle();
            return 0;
        }
        catch (const std::exception& failure)
        {
            std::fprintf(stderr, "%s: FMINUS_WEB_ALLOW: %s\n", name, failure.what());
            return 2;
        }
    }

    void fminus_web_main_stop(void* context) noexcept
    {
        if (context == nullptr || executableContext == nullptr || executableContext->handle() != context)
            return;
        delete executableContext;
        executableContext = nullptr;
    }
}
