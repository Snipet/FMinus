# cmake/FMinusPins.cmake: every external dependency, pinned by version, URL and SHA-256. All of them are fetched with
# FetchContent; nothing is taken from the system unless an FMINUS_*_MODE option asks for it.
#
# Bumping a pin: change the version, URL and hash together. Scripts/pin-llvm.sh <version> prints the LLVM lines from
# the GitHub release (asset size and digest). A changed LLVM pin or option set builds a new LLVM flavor next to the old
# one in the deps cache (cmake/FMinusLLVM.cmake); delete the old one by hand when you no longer need it.

# LLVM: the monorepo source release. Only llvm/ is built (no clang, no runtimes).
set(FMINUS_LLVM_VERSION "23.1.3")
set(FMINUS_LLVM_URL
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-${FMINUS_LLVM_VERSION}/llvm-project-${FMINUS_LLVM_VERSION}.src.tar.xz")
set(FMINUS_LLVM_SHA256 "c44186a7762ed28954be72e5ff6df9808e0779d4f1bf014ecc4e7e211d31ee34")   # 179,220,548 bytes

# GNU Bison: built from source at configure time (macOS ships 2.3, too old for the C++ variant skeleton).
set(FMINUS_BISON_VERSION "3.8.2")
set(FMINUS_BISON_URLS
    "https://ftpmirror.gnu.org/gnu/bison/bison-${FMINUS_BISON_VERSION}.tar.xz"
    "https://ftp.gnu.org/gnu/bison/bison-${FMINUS_BISON_VERSION}.tar.xz")
set(FMINUS_BISON_SHA256 "9bba0214ccf7f1079c5d59210045227bcf619519840ebfa80cd3849cff5a5bf2")

# Flex: built from source at configure time, so every machine runs the same scanner generator.
set(FMINUS_FLEX_VERSION "2.6.4")
set(FMINUS_FLEX_URL "https://github.com/westes/flex/releases/download/v${FMINUS_FLEX_VERSION}/flex-${FMINUS_FLEX_VERSION}.tar.gz")
set(FMINUS_FLEX_SHA256 "e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995")

# CLI11: command-line parsing for the executables under tools/ (header-only; the release's source package).
set(FMINUS_CLI11_VERSION "2.7.2")
set(FMINUS_CLI11_URL
    "https://github.com/CLIUtils/CLI11/releases/download/v${FMINUS_CLI11_VERSION}/CLI11-${FMINUS_CLI11_VERSION}-Source.tar.gz")
set(FMINUS_CLI11_SHA256 "0cb0ef44c4c7129ea972505ee81e6d09c71b2e7fe59ca3a13c47d8a994d682b2")   # 314,257 bytes

# Catch2 v3: unit and smoke tests (tests/).
set(FMINUS_CATCH2_VERSION "3.16.1")
set(FMINUS_CATCH2_URL "https://github.com/catchorg/Catch2/archive/refs/tags/v${FMINUS_CATCH2_VERSION}.tar.gz")
set(FMINUS_CATCH2_SHA256 "d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d")   # 1,228,880 bytes
