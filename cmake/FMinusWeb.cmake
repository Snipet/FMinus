# cmake/FMinusWeb.cmake: libcurl and its TLS library, for the web library fminus_web (src/web/). Only when FMINUS_WEB is
# on: with it off, nothing is fetched or built, and there is no target.
#
# Both are built at configure time into the deps cache, as cmake/FMinusParserTools.cmake builds Bison and Flex: the
# pinned sources (cmake/FMinusPins.cmake) are fetched once into ${FMINUS_DEPS_DIR}/src/<name>-<version>, marked with
# their hash; each is configured, built and installed with its own CMake into ${FMINUS_DEPS_DIR}/install/<name>-
# <version>-<flavor>, and every later configure of any build tree finds the install there. The flavor is a hash of
# what the build was given (the C compiler, the TLS library, the options below), so a change builds a new install
# beside the old one. Their CMake is never added to FMinus's build: each is its own project, with its own flags and
# none of FMinus's warnings or sanitizers.
#
# TLS: Schannel on Windows (the system's, and its certificate store: nothing to fetch); Mbed TLS, the 4.1 LTS branch,
# on macOS and Linux, with its threading on (pthreads: the I/O thread and a host's own use of the library may run at
# once). libcurl is built static, without its executable, docs, tests or examples, with HTTP and HTTPS and nothing else:
# every other protocol, cookies, .netrc, alt-svc, HSTS, proxies, authentication, MIME and forms, DoH, WebSockets (for
# now), IDN, PSL, compression and HTTP/2 off, and no certificate bundle compiled in (src/web/ names one at run time).
# Both are compiled with hidden visibility (curl's own CURL_HIDDEN_SYMBOLS, which marks its API default, is off), so a
# plugin that links FMinus exports none of their symbols, and as position-independent code, for the Python package.
#
# Gives the imported targets fminus_web_curl (libcurl, with CURL_STATICLIB and what it links: Mbed TLS's libraries,
# Threads, the system libraries) and, with Mbed TLS, fminus_web_mbedtls (its three libraries and its headers, which
# this tree's web tests also use, for a certificate authority and a TLS server of their own), FMINUS_WEB_TLS
# (schannel or mbedtls), and FMINUS_WEB_LINK: what an executable that calls the web library links after
# libfminus_web.a (support/Aot.cpp), libcurl's and Mbed TLS's archives in link order and then the system libraries
# libcurl's own CMake links on that platform (its CURL_LIBS: pthreads on Linux; SystemConfiguration, CoreFoundation and
# CoreServices on macOS, with Security for web/RootsMac.cpp; ws2_32, iphlpapi, bcrypt, advapi32, crypt32 and secur32
# on Windows).
if(NOT FMINUS_WEB)
  return()
endif()

include(FetchContent)

if(NOT FMINUS_JSON AND NOT FMINUS_XML)
  message(NOTICE "FMinus: FMINUS_WEB is ON with FMINUS_JSON and FMINUS_XML both OFF: a program can fetch a response, but "
                 "read it only as raw bytes (bytes_floats, bytes_pcm16). Probably not what was meant.")
endif()

if(WIN32)
  set(FMINUS_WEB_TLS schannel)
else()
  set(FMINUS_WEB_TLS mbedtls)
endif()

# Fetches <name>-<version> into the deps cache (populate only) and sets <out> to its source directory.
function(_fminus_web_source name version sha256 out)
  cmake_parse_arguments(PARSE_ARGV 4 _a "" "" "URLS")
  set(_src "${FMINUS_DEPS_DIR}/src/${name}-${version}")
  file(LOCK "${_src}.lock" GUARD FUNCTION)
  set(${out} "${_src}" PARENT_SCOPE)
  if(EXISTS "${_src}/.fminus-populated")
    file(READ "${_src}/.fminus-populated" _marker)
    if(_marker STREQUAL sha256)
      return()
    endif()
  endif()
  message(STATUS "FMinus: fetching ${name} ${version} into ${_src}")
  FetchContent_Declare(fminus_${name}
    URL ${_a_URLS}
    URL_HASH SHA256=${sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_DIR "${_src}"
    SOURCE_SUBDIR fminus-sources-only)   # no such directory: populate, but add none of its CMake
  FetchContent_MakeAvailable(fminus_${name})
  file(WRITE "${_src}/.fminus-populated" "${sha256}")
endfunction()

# Configures, builds and installs the CMake project at <source> into the deps cache, as Release, with <args>, unless
# its flavor is installed already. Sets <out> to the install's prefix.
function(_fminus_web_build name version source out)
  cmake_parse_arguments(PARSE_ARGV 4 _a "" "" "ARGS")
  fminus_dependency_generator(_gen)
  set(_common
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_C_VISIBILITY_PRESET=hidden
    -DCMAKE_POLICY_DEFAULT_CMP0063=NEW    # the visibility preset for static libraries too
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW)   # CMAKE_MSVC_RUNTIME_LIBRARY
  if(CMAKE_MSVC_RUNTIME_LIBRARY)
    list(APPEND _common "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}")
  endif()
  if(APPLE)
    list(APPEND _common "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(CMAKE_OSX_ARCHITECTURES)
      list(JOIN CMAKE_OSX_ARCHITECTURES "\\;" _archs)
      list(APPEND _common "-DCMAKE_OSX_ARCHITECTURES=${_archs}")
    endif()
  endif()
  string(SHA256 _flavor "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} ${_common} ${_a_ARGS}")
  string(SUBSTRING "${_flavor}" 0 10 _flavor)
  set(_prefix "${FMINUS_DEPS_DIR}/install/${name}-${version}-${_flavor}")
  set(${out} "${_prefix}" PARENT_SCOPE)
  set(_stamp "${_prefix}/.fminus-installed")
  if(EXISTS "${_stamp}")
    return()
  endif()
  file(LOCK "${_prefix}.lock" GUARD FUNCTION)
  if(EXISTS "${_stamp}")
    return()
  endif()

  message(STATUS "FMinus: building ${name} ${version} into ${_prefix}")
  set(_bdir "${FMINUS_DEPS_DIR}/build/${name}-${version}-${_flavor}")
  file(REMOVE_RECURSE "${_bdir}")
  file(MAKE_DIRECTORY "${_bdir}")
  # A dependency: the environment's flags (and this tree's warnings and sanitizers) stay out of it.
  set(_env "${CMAKE_COMMAND}" -E env --unset=CFLAGS --unset=CPPFLAGS --unset=CXXFLAGS --unset=LDFLAGS)
  foreach(_step configure build install)
    if(_step STREQUAL "configure")
      set(_cmd ${_env} "${CMAKE_COMMAND}" ${_gen} -S "${source}" -B "${_bdir}" "-DCMAKE_INSTALL_PREFIX=${_prefix}"
               ${_common} ${_a_ARGS})
    elseif(_step STREQUAL "build")
      set(_cmd ${_env} "${CMAKE_COMMAND}" --build "${_bdir}" --config Release --parallel ${FMINUS_DEPS_JOBS})
    else()
      set(_cmd ${_env} "${CMAKE_COMMAND}" --install "${_bdir}" --config Release)
    endif()
    execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc
                    OUTPUT_FILE "${_bdir}/fminus-${_step}.log" ERROR_FILE "${_bdir}/fminus-${_step}.log")
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "FMinus: ${name} ${version}: ${_step} failed (${_rc}); log: ${_bdir}/fminus-${_step}.log")
    endif()
  endforeach()
  file(WRITE "${_stamp}" "${_flavor}")
endfunction()

# ---- Mbed TLS (macOS, Linux) ----------------------------------------------------------------------------------------
set(_fminus_curl_tls_args "")
if(FMINUS_WEB_TLS STREQUAL "mbedtls")
  _fminus_web_source(mbedtls "${FMINUS_MBEDTLS_VERSION}" "${FMINUS_MBEDTLS_SHA256}" _fminus_mbedtls_src
                     URLS "${FMINUS_MBEDTLS_URL}")
  # TF-PSA-Crypto's user configuration, appended to its default one: threading, on pthreads.
  set(_fminus_mbedtls_config "${FMINUS_DEPS_DIR}/src/fminus-mbedtls-${FMINUS_MBEDTLS_VERSION}-config.h")
  file(CONFIGURE OUTPUT "${_fminus_mbedtls_config}" CONTENT
       "/* FMinus (cmake/FMinusWeb.cmake): appended to TF-PSA-Crypto's default configuration. */\n#define MBEDTLS_THREADING_C\n#define MBEDTLS_THREADING_PTHREAD\n")
  _fminus_web_build(mbedtls "${FMINUS_MBEDTLS_VERSION}" "${_fminus_mbedtls_src}" _fminus_mbedtls_prefix ARGS
    -DENABLE_PROGRAMS=OFF
    -DENABLE_TESTING=OFF
    -DGEN_FILES=OFF
    -DMBEDTLS_FATAL_WARNINGS=OFF
    -DTF_PSA_CRYPTO_FATAL_WARNINGS=OFF
    -DUSE_STATIC_MBEDTLS_LIBRARY=ON
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
    -DLINK_WITH_PTHREAD=ON
    "-DTF_PSA_CRYPTO_USER_CONFIG_FILE=${_fminus_mbedtls_config}")
  # The install of this flavor, never one an earlier configure cached (find_package reads <name>_DIR first).
  unset(MbedTLS_DIR CACHE)
  unset(TF-PSA-Crypto_DIR CACHE)
  find_package(MbedTLS CONFIG REQUIRED PATHS "${_fminus_mbedtls_prefix}" NO_DEFAULT_PATH)
  set(MbedTLS_DIR "${MbedTLS_DIR}" CACHE PATH "Mbed TLS (FMinus's deps cache)" FORCE)
  add_library(fminus_web_mbedtls INTERFACE IMPORTED)
  target_link_libraries(fminus_web_mbedtls INTERFACE MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::tfpsacrypto)
  set(_fminus_curl_tls_args
    -DCURL_USE_MBEDTLS=ON
    -DCURL_USE_CMAKECONFIG=ON
    "-DMbedTLS_DIR=${MbedTLS_DIR}")
else()
  set(_fminus_curl_tls_args -DCURL_USE_SCHANNEL=ON)
endif()

# ---- libcurl --------------------------------------------------------------------------------------------------------
_fminus_web_source(curl "${FMINUS_CURL_VERSION}" "${FMINUS_CURL_SHA256}" _fminus_curl_src URLS ${FMINUS_CURL_URLS})
_fminus_web_build(curl "${FMINUS_CURL_VERSION}" "${_fminus_curl_src}" _fminus_curl_prefix ARGS
  ${_fminus_curl_tls_args}
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF
  -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DCURL_HIDDEN_SYMBOLS=OFF
  -DCURL_WERROR=OFF -DPICKY_COMPILER=OFF -DCURL_LTO=OFF -DENABLE_DEBUG=OFF -DCURL_USE_PKGCONFIG=OFF
  -DCURL_ENABLE_SSL=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_GNUTLS=OFF -DCURL_USE_WOLFSSL=OFF -DCURL_USE_RUSTLS=OFF
  -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF -DCURL_CA_NATIVE=OFF -DCURL_DISABLE_CA_SEARCH=ON
  -DUSE_APPLE_SECTRUST=OFF
  -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON -DCURL_DISABLE_GOPHER=ON
  -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_POP3=ON
  -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON
  -DCURL_DISABLE_COOKIES=ON -DCURL_DISABLE_NETRC=ON -DCURL_DISABLE_ALTSVC=ON -DCURL_DISABLE_HSTS=ON
  -DCURL_DISABLE_PROXY=ON -DCURL_DISABLE_HTTP_AUTH=ON -DCURL_DISABLE_BASIC_AUTH=ON -DCURL_DISABLE_BEARER_AUTH=ON
  -DCURL_DISABLE_DIGEST_AUTH=ON -DCURL_DISABLE_KERBEROS_AUTH=ON -DCURL_DISABLE_NEGOTIATE_AUTH=ON
  -DCURL_DISABLE_AWS=ON -DCURL_ENABLE_NTLM=OFF -DCURL_DISABLE_MIME=ON -DCURL_DISABLE_FORM_API=ON
  -DCURL_DISABLE_DOH=ON -DCURL_DISABLE_WEBSOCKETS=ON -DCURL_DISABLE_IPFS=ON -DCURL_DISABLE_BINDLOCAL=ON
  -DCURL_DISABLE_LIBCURL_OPTION=ON -DCURL_DISABLE_PROGRESS_METER=ON -DCURL_DISABLE_GETOPTIONS=ON
  -DCURL_DISABLE_HTTPSIG=ON -DCURL_DISABLE_INSTALL=OFF
  -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=OFF -DUSE_APPLE_IDN=OFF -DCURL_USE_LIBSSH2=OFF
  -DCURL_USE_LIBSSH=OFF -DCURL_USE_GSSAPI=OFF -DCURL_USE_GSASL=OFF -DCURL_USE_LIBUV=OFF -DCURL_USE_LIBBACKTRACE=OFF
  -DUSE_NGHTTP2=OFF -DUSE_NGTCP2=OFF -DUSE_QUICHE=OFF -DUSE_HTTPSRR=OFF -DUSE_ECH=OFF -DUSE_SSLS_EXPORT=OFF
  -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DENABLE_ARES=OFF -DENABLE_THREADED_RESOLVER=ON
  -DENABLE_UNIX_SOCKETS=OFF -DENABLE_IPV6=ON)
unset(CURL_DIR CACHE)   # this flavor's install, as for Mbed TLS above
find_package(CURL CONFIG REQUIRED PATHS "${_fminus_curl_prefix}" NO_DEFAULT_PATH)
set(CURL_DIR "${CURL_DIR}" CACHE PATH "libcurl (FMinus's deps cache)" FORCE)
if(NOT TARGET CURL::libcurl_static)
  message(FATAL_ERROR "FMinus: libcurl ${FMINUS_CURL_VERSION} in ${_fminus_curl_prefix} has no static library")
endif()
add_library(fminus_web_curl INTERFACE IMPORTED)
target_link_libraries(fminus_web_curl INTERFACE CURL::libcurl_static)

# An imported static library's file, for FMINUS_WEB_LINK.
function(_fminus_web_archive target out)
  get_target_property(_file ${target} IMPORTED_LOCATION_RELEASE)
  if(NOT _file)
    get_target_property(_file ${target} IMPORTED_LOCATION)
  endif()
  if(NOT _file)
    message(FATAL_ERROR "FMinus: ${target} has no imported library file")
  endif()
  set(${out} "${_file}" PARENT_SCOPE)
endfunction()
_fminus_web_archive(CURL::libcurl_static _fminus_curl_archive)
set(FMINUS_WEB_LINK "${_fminus_curl_archive}")
if(FMINUS_WEB_TLS STREQUAL "mbedtls")
  foreach(_lib MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::tfpsacrypto)
    _fminus_web_archive(${_lib} _archive)
    list(APPEND FMINUS_WEB_LINK "${_archive}")
  endforeach()
endif()
if(WIN32)
  list(APPEND FMINUS_WEB_LINK ws2_32.lib iphlpapi.lib bcrypt.lib advapi32.lib crypt32.lib secur32.lib)
elseif(APPLE)
  list(APPEND FMINUS_WEB_LINK -framework SystemConfiguration -framework CoreFoundation -framework CoreServices
                              -framework Security)
else()
  list(APPEND FMINUS_WEB_LINK -pthread)
endif()
message(STATUS "FMinus: the web library: libcurl ${FMINUS_CURL_VERSION} with ${FMINUS_WEB_TLS} (${_fminus_curl_prefix})")
