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

namespace
{
    // The executable's one context, whose handle its program's global holds: fminus_web_main_stop destroys it.
    fminus::web::Context* executableContext = nullptr;
} // namespace

extern "C"
{
    std::int32_t fminus_web_main_start(void** context, const char* program) noexcept
    {
        *context = nullptr;
        const char* name = program != nullptr && *program != '\0' ? program : "fminus";
        try
        {
            fminus::web::Policy policy;
            if (const char* allow = std::getenv("FMINUS_WEB_ALLOW"); allow != nullptr)
            {
                std::string pattern;
                for (const char* c = allow;; ++c)
                {
                    if (*c == '\0' || *c == ' ' || *c == ',' || *c == '\n' || *c == '\t' || *c == '\r')
                    {
                        if (!pattern.empty())
                            policy.allow.push_back(pattern);
                        pattern.clear();
                        if (*c == '\0')
                            break;
                    }
                    else
                        pattern += *c;
                }
            }
            if (policy.allow.empty())
                return 0;   // the default: no network
            if (const char* caFile = std::getenv("FMINUS_WEB_CA_FILE"); caFile != nullptr)
                policy.caFile = caFile;
            std::string error;
            std::unique_ptr<fminus::web::Context> made = fminus::web::Context::create(policy, error);
            if (made == nullptr)
            {
                std::fprintf(stderr, "%s: FMINUS_WEB_ALLOW: %s\n", name, error.c_str());
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
