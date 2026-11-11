// src/web/Handle.h: what every handle the web library gives a program begins with, its kind. A request made
// (http_get, http_post), a request received (the server's, http_next and http_accept) and a server (http_listen) are
// all a void* to the program, which can hand any of them to any http_ builtin; each C function reads the kind first and
// does nothing for a handle of another (Web.h: FMINUS_WEB_INVALID, 0 or null), where a wrong handle would otherwise be
// read as the right one. The kinds are distinct 32-bit words rather than 0, 1 and 2, so that memory that was never a
// handle is unlikely to pass for one (it is still a program's bug to pass one: the library cannot check every pointer).
//
// A handle reaches the program as a pointer to its Handle, converted to void* (toHandle), and comes back the same way
// (as<T>), so the conversions are the base's and never depend on where a derived type puts it.
#pragma once

#include <cstdint>

namespace fminus::web
{
    enum class Kind : std::uint32_t
    {
        Made = 0x464d5271,       // "FMRq": a request a program made (Web.cpp's Request)
        Received = 0x464d5272,   // "FMRr": a request a program's server received (Server.cpp's Slot)
        Server = 0x464d5273,     // "FMRs": a program's server (Server.cpp's Server)
    };

    struct Handle
    {
        explicit Handle(Kind k) noexcept
            : kind(k)
        {
        }
        const Kind kind;
    };

    // What the program is given for `handle`.
    inline void* toHandle(Handle* handle) noexcept
    {
        return handle;
    }

    // The handle as a T (whose kKind is its kind), or null when it is null or of another kind.
    template <typename T>
    T* as(void* handle) noexcept
    {
        if (handle == nullptr)
            return nullptr;
        Handle* base = static_cast<Handle*>(handle);
        return base->kind == T::kKind ? static_cast<T*>(base) : nullptr;
    }

    template <typename T>
    const T* as(const void* handle) noexcept
    {
        if (handle == nullptr)
            return nullptr;
        const Handle* base = static_cast<const Handle*>(handle);
        return base->kind == T::kKind ? static_cast<const T*>(base) : nullptr;
    }
} // namespace fminus::web
