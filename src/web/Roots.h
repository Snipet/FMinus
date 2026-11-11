// src/web/Roots.h: the root certificates a request trusts when the policy names no file (Policy::caFile), one way on
// each platform, each in its own file: RootsWindows.cpp (Schannel reads the Windows store itself), RootsMac.cpp (the
// Keychain's roots, loaded once and handed to libcurl as PEM), RootsLinux.cpp (every other system: the usual bundle
// paths, probed once at run time, since a path compiled in would be the build machine's).
#pragma once

typedef void CURL;

namespace fminus::web
{
    // Gives the request `easy` the system's roots. False when there are none to give (no bundle found, or the
    // Keychain gave none): an https request then fails as FMINUS_WEB_CERTIFICATE. Called on the I/O thread only.
    bool useSystemRoots(CURL* easy);
} // namespace fminus::web
