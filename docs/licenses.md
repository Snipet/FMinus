# Licenses

FMinus is free software under the GNU General Public License, version 3 (`LICENSE`). Its dependencies keep their own
licenses: LLVM (Apache 2.0 with LLVM exceptions), PFFFT (FFTPACKv5's BSD-style license: a binary that includes it, as
anything linking FMinus's runtime does, reproduces its copyright notice, conditions and disclaimer, from its
`LICENSE.txt`, in its documentation), yyjson (MIT: a binary that includes it, as anything linking FMinus's runtime
built with `FMINUS_JSON` does, as it is by default, carries its copyright notice and permission notice, from its
`LICENSE`), pugixml (MIT: a binary that includes it, as anything linking FMinus's runtime built with `FMINUS_XML`
does, as it is by default, carries its copyright notice and permission notice, from its `LICENSE.md`), fast_float
(Apache-2.0, MIT or BSL-1.0, at the user's choice; under BSL-1.0, a binary that includes it, as
anything linking FMinus's runtime built with `FMINUS_JSON` or `FMINUS_XML` does, needs no notice), libcurl (the curl
license, MIT-like: a binary that includes it, as anything linking FMinus built with `FMINUS_WEB` does, carries its
copyright notice and permission notice, from its `COPYING`), Mbed TLS (Apache-2.0 or GPL-2.0-or-later, at the user's
choice; under Apache-2.0, a binary that includes it, as anything linking FMinus built with `FMINUS_WEB` does on macOS
and Linux, carries a copy of the license, from its `LICENSE`), llhttp (MIT: a binary that includes it, as anything
linking FMinus built with `FMINUS_WEB_SERVER` does, carries its copyright notice and permission notice, from its
`LICENSE`), Catch2 (BSL-1.0),
CLI11 (BSD-3-Clause) and pybind11 (BSD-3-Clause; the Python package only). JUCE, which only `examples/plugin/` uses, is
under the AGPLv3 or JUCE's commercial license; that example's README says what that means for a plugin built from it.
Bison and Flex run only at build time; the code they generate is covered by Bison's skeleton exception and Flex's
permissive license. On Windows they are WinFlexBison's ports, under the same licenses (its build scripts are
GPL-3.0-or-later).

## What an install carries

An installed FMinus ([building.md](building.md), "Installing") is a distribution of these binaries: `bin/fminus` links
LLVM and CLI11 in, besides the runtime's code, and `lib/fminus/`'s archives hold the code every executable `fminus`
makes links. So an install carries every notice its files need, in `share/doc/fminus/`: `LICENSE`, FMinus's own;
`NOTICES.txt`, which names each dependency the install holds, its license, and which installed file holds its code;
and `third-party/<name>/`, each dependency's own license file. A distribution of an executable `fminus` made carries
the notices `NOTICES.txt` lists for "every executable fminus makes".
