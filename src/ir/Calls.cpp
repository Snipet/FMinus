// src/ir/Calls.cpp: calls. Inside a method, a method of its module first (`tick(x)` is this->tick(x), as in C++,
// where the class's names hide the others); then the builtins, in the order a call tries them: the reductions,
// poly_lanes and poly_index, poly_load and poly_store, <math.h>'s float functions (LLVM intrinsics, overloaded on
// vectors; the transcendental ones the runtime's functions, by the math mode: Math.cpp), <stdlib.h>'s heap (malloc,
// calloc and free) and the FFT (fft_create, fft_forward, ...), both on the runtime's functions, and hash and randf
// (inline: Hash.cpp); then the program's own functions. The runtime's other builtins (bytes_floats, bytes_pcm16, the
// JSON parser's json_parse, json_float, ..., the XML parser's xml_parse, xml_float, ..., the web library's http_get,
// http_wait, ..., its server's http_listen, http_next, ..., the file interface's file_read, file_list, ..., and those
// that take string literals: Strings.cpp, Paths.cpp, Web.cpp, WebServer.cpp, Files.cpp) come before the rest, as a
// method of the module does, since their arguments are lowered as their parameters say (a literal is one only there).
// The runtime's functions are declared with what they do (describeRuntime), so LLVM does not take one for an unknown
// function that may read and write all memory.
#include "ir/Lowering.h"

#include "fminus/Config.h"   // FMINUS_HAS_JSON, FMINUS_HAS_XML, FMINUS_HAS_WEB, FMINUS_HAS_WEB_SERVER,
                             // FMINUS_HAS_FILES, FMINUS_HAS_FILES_WRITE: which build options this build has

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/ModRef.h>

#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace fminus::ir::detail
{
    // <math.h>'s float functions -> the LLVM intrinsic each lowers to, overloaded on float, and for the transcendental
    // ones the stem of the runtime functions the vector and libm math modes call instead (Math.cpp). fabsf, sqrtf,
    // floorf, fminf and fmaxf become instructions, exact in every mode; sinf, cosf, expf, logf, tanhf and tanf have
    // none. Then <math.h>'s double functions that are instructions too, exact: sqrt, fabs, floor, fmin, fmax and fma
    // (a fused multiply-add, rounded once, as C's fma is: FMinus contracts nothing else into one). FMinus has no double
    // sin, cos, ...: its kernels are a float's, and a double's would be libm's (lowerCall's note says how to call it).
    constexpr std::array kBuiltins{
        Builtin{"fabsf", llvm::Intrinsic::fabs, 1},        Builtin{"sqrtf", llvm::Intrinsic::sqrt, 1},
        Builtin{"sinf", llvm::Intrinsic::sin, 1, "sin"},   Builtin{"cosf", llvm::Intrinsic::cos, 1, "cos"},
        Builtin{"expf", llvm::Intrinsic::exp, 1, "exp"},   Builtin{"logf", llvm::Intrinsic::log, 1, "log"},
        Builtin{"floorf", llvm::Intrinsic::floor, 1},      Builtin{"fminf", llvm::Intrinsic::minnum, 2},
        Builtin{"fmaxf", llvm::Intrinsic::maxnum, 2},      Builtin{"tanhf", llvm::Intrinsic::tanh, 1, "tanh"},
        Builtin{"tanf", llvm::Intrinsic::tan, 1, "tan"},
        Builtin{"sqrt", llvm::Intrinsic::sqrt, 1, {}, ast::Kind::Double},
        Builtin{"fabs", llvm::Intrinsic::fabs, 1, {}, ast::Kind::Double},
        Builtin{"floor", llvm::Intrinsic::floor, 1, {}, ast::Kind::Double},
        Builtin{"fmin", llvm::Intrinsic::minnum, 2, {}, ast::Kind::Double},
        Builtin{"fmax", llvm::Intrinsic::maxnum, 2, {}, ast::Kind::Double},
        Builtin{"fma", llvm::Intrinsic::fma, 3, {}, ast::Kind::Double},
    };

    namespace
    {
        // <math.h>'s double functions whose float ones FMinus computes with its own kernels (sinf, ...): what the
        // error for a call of one without a prototype explains.
        constexpr std::array<std::string_view, 6> kLibmDoubles{"sin", "cos", "tan", "exp", "log", "tanh"};
    } // namespace

    // C's heap, <stdlib.h>'s functions, on the runtime's (runtime/Runtime.h): memory aligned to 64 bytes and zeroed.
    constexpr std::array kAllocations{
        Allocation{"malloc", "fminus_rt_malloc", 1},
        Allocation{"calloc", "fminus_rt_calloc", 2},
        Allocation{"free", "fminus_rt_free", 1},
    };

    // The FFT, on the runtime's (runtime/Fft.cpp): a plan for one size, made off the audio thread (it allocates its
    // tables and buffers, as calloc does), the transforms on it, which allocate nothing and may run on the audio
    // thread, and the plan's end. A real plan transforms n floats into n/2 + 1 bins, float2s (re, im), and back; a
    // complex plan n float2s into n, and back. Neither direction scales: inverse(forward(x)) is n times x.
    constexpr std::array kFfts{
        Fft{"fft_create", 1, true},  Fft{"fft_create_complex", 1, true}, Fft{"fft_destroy", 1, true},
        Fft{"fft_forward", 3, false}, Fft{"fft_inverse", 3, false},
    };

    // The runtime's other builtins (runtime/Bytes.cpp), each a function of the runtime with parameters as Param says
    // and an int result, and the build option it needs. bytes_floats(data, bytes, out, cap) and bytes_pcm16 decode
    // little-endian float32, or 16-bit PCM scaled to [-1, 1), into floats: how many values the data holds, at most cap
    // of them written; wait-free, so a nonblocking function may call them. __fminus_string_hash(format, ...) and
    // __fminus_string_hash_blocking are scaffolding, names C reserves for the implementation: the hash of the text a
    // format and its ints make, which proves a literal's way from the source to the runtime
    // (tests/unit/StringTest.cpp), the second classed as blocking. They stay: the builtins that take formats and block
    // for real are each a build option's (json_float's paths FMINUS_JSON's, http_wait FMINUS_WEB's, off by default), so
    // in the default build nothing else carries a blocking call through the effects, and in one without the parsers
    // nothing else carries a format; these test both in every build (tests/unit/StringTest.cpp).
    constexpr std::array kRuntimeBuiltins{
        RuntimeBuiltin{"bytes_floats", "fminus_rt_bytes_floats", {Param::Bytes, Param::Int, Param::Floats, Param::Int},
                       4, RuntimeEffect::WaitFree},
        RuntimeBuiltin{"bytes_pcm16", "fminus_rt_bytes_pcm16", {Param::Bytes, Param::Int, Param::Floats, Param::Int},
                       4, RuntimeEffect::WaitFree},
        RuntimeBuiltin{"__fminus_string_hash", "fminus_rt_string_hash", {Param::Format}, 1, RuntimeEffect::WaitFree},
        RuntimeBuiltin{"__fminus_string_hash_blocking", "fminus_rt_string_hash", {Param::Format}, 1,
                       RuntimeEffect::Blocks},
        // The JSON parser (runtime/Json.cpp), in a build with FMINUS_JSON. json_parse(data, bytes) gives a document, a
        // json_doc (null for a text that is no JSON, too large or nested too deeply), and json_free(doc) frees it: both
        // allocate. The reads take the document, a JSON Pointer with places for ints (a path, Paths.cpp) and its
        // ints, and give what is there: json_float, json_double, json_int and json_bool the value, or their last
        // argument, the fallback, when there is none of that kind; json_count an array's or an object's size;
        // json_floats(doc, path, out, cap) an array of numbers, or of rows of them; json_choice(doc, path, "a", "b",
        // ...) the index of the literal a string equals, or -1; json_base64_floats(doc, path, out, cap) a base64
        // string's float32s. A null document is an empty one. Every read is wait-free.
        RuntimeBuiltin{"json_parse", "fminus_rt_json_parse", {Param::Bytes, Param::Int}, 2, RuntimeEffect::Allocates,
                       Feature::Json, Result::JsonDoc},
        RuntimeBuiltin{"json_free", "fminus_rt_json_free", {Param::JsonDoc}, 1, RuntimeEffect::Allocates, Feature::Json,
                       Result::Void},
        RuntimeBuiltin{"json_float", "fminus_rt_json_float", {Param::JsonRead, Param::Path, Param::Float}, 3,
                       RuntimeEffect::WaitFree, Feature::Json, Result::Float},
        RuntimeBuiltin{"json_double", "fminus_rt_json_double", {Param::JsonRead, Param::Path, Param::Double}, 3,
                       RuntimeEffect::WaitFree, Feature::Json, Result::Double},
        RuntimeBuiltin{"json_int", "fminus_rt_json_int", {Param::JsonRead, Param::Path, Param::Int}, 3,
                       RuntimeEffect::WaitFree, Feature::Json, Result::Int},
        RuntimeBuiltin{"json_bool", "fminus_rt_json_bool", {Param::JsonRead, Param::Path, Param::Bool}, 3,
                       RuntimeEffect::WaitFree, Feature::Json, Result::Bool},
        RuntimeBuiltin{"json_count", "fminus_rt_json_count", {Param::JsonRead, Param::Path}, 2, RuntimeEffect::WaitFree,
                       Feature::Json, Result::Int},
        RuntimeBuiltin{"json_floats", "fminus_rt_json_floats",
                       {Param::JsonRead, Param::Path, Param::Floats, Param::Int}, 4, RuntimeEffect::WaitFree,
                       Feature::Json, Result::Int},
        RuntimeBuiltin{"json_choice", "fminus_rt_json_choice", {Param::JsonRead, Param::Path, Param::Choices}, 3,
                       RuntimeEffect::WaitFree, Feature::Json, Result::Int},
        RuntimeBuiltin{"json_base64_floats", "fminus_rt_json_base64_floats",
                       {Param::JsonRead, Param::Path, Param::Floats, Param::Int}, 4, RuntimeEffect::WaitFree,
                       Feature::Json, Result::Int},
        // The XML parser (runtime/Xml.cpp), in a build with FMINUS_XML, made as the JSON parser's are. xml_parse(data,
        // bytes) gives a document, an xml_doc (null for a text that is no XML, too large or nested too deeply), and
        // xml_free(doc) frees it: both allocate. The reads take the document, an XML path with places for ints
        // (Paths.cpp: "/preset/osc[%d]/@detune", an index from 0) and its ints, and read the attribute's value or the
        // element's text: xml_float, xml_double, xml_int and xml_bool the value, or their fallback; xml_count how many
        // elements the last step matches; xml_floats(doc, path, out, cap) a list of numbers; xml_choice(doc, path,
        // "a", "b", ...) the index of the literal it equals, or -1. A null document is an empty one. Every read is
        // wait-free.
        RuntimeBuiltin{"xml_parse", "fminus_rt_xml_parse", {Param::Bytes, Param::Int}, 2, RuntimeEffect::Allocates,
                       Feature::Xml, Result::XmlDoc},
        RuntimeBuiltin{"xml_free", "fminus_rt_xml_free", {Param::XmlDoc}, 1, RuntimeEffect::Allocates, Feature::Xml,
                       Result::Void},
        RuntimeBuiltin{"xml_float", "fminus_rt_xml_float", {Param::XmlRead, Param::XmlPath, Param::Float}, 3,
                       RuntimeEffect::WaitFree, Feature::Xml, Result::Float},
        RuntimeBuiltin{"xml_double", "fminus_rt_xml_double", {Param::XmlRead, Param::XmlPath, Param::Double}, 3,
                       RuntimeEffect::WaitFree, Feature::Xml, Result::Double},
        RuntimeBuiltin{"xml_int", "fminus_rt_xml_int", {Param::XmlRead, Param::XmlPath, Param::Int}, 3,
                       RuntimeEffect::WaitFree, Feature::Xml, Result::Int},
        RuntimeBuiltin{"xml_bool", "fminus_rt_xml_bool", {Param::XmlRead, Param::XmlPath, Param::Bool}, 3,
                       RuntimeEffect::WaitFree, Feature::Xml, Result::Bool},
        RuntimeBuiltin{"xml_count", "fminus_rt_xml_count", {Param::XmlRead, Param::XmlPath}, 2,
                       RuntimeEffect::WaitFree, Feature::Xml, Result::Int},
        RuntimeBuiltin{"xml_floats", "fminus_rt_xml_floats",
                       {Param::XmlRead, Param::XmlPath, Param::Floats, Param::Int}, 4, RuntimeEffect::WaitFree,
                       Feature::Xml, Result::Int},
        RuntimeBuiltin{"xml_choice", "fminus_rt_xml_choice", {Param::XmlRead, Param::XmlPath, Param::Choices}, 3,
                       RuntimeEffect::WaitFree, Feature::Xml, Result::Int},
        // The web library (src/web/Web.h), in a build with FMINUS_WEB. http_get(url, ...) and http_post(url, data,
        // bytes, content_type, ...) start a request under the program's web context (Web.cpp) and give its handle, an
        // http_response, at once: the URL a literal with places for the ints after every other argument, content_type
        // a literal. http_wait(r, timeout_ms) blocks until it is done or the time is up and gives its status;
        // http_status, http_size, http_body (a const void*) and http_progress read it, wait-free; http_cancel stops it
        // and http_close frees it, which allocate (they wait for the library's I/O thread, and close frees). A null
        // handle does nothing, and reads as a failed request (HTTP_INVALID, 0, null).
        RuntimeBuiltin{"http_get", "fminus_web_http_get", {Param::Url}, 1, RuntimeEffect::Allocates, Feature::Web,
                       Result::Response},
        RuntimeBuiltin{"http_post", "fminus_web_http_post", {Param::Url, Param::Bytes, Param::Int, Param::ContentType},
                       4, RuntimeEffect::Allocates, Feature::Web, Result::Response},
        RuntimeBuiltin{"http_wait", "fminus_web_http_wait", {Param::Response, Param::Int}, 2, RuntimeEffect::Blocks,
                       Feature::Web, Result::Int},
        RuntimeBuiltin{"http_status", "fminus_web_http_status", {Param::Response}, 1, RuntimeEffect::WaitFree,
                       Feature::Web, Result::Int},
        RuntimeBuiltin{"http_size", "fminus_web_http_size", {Param::Message}, 1, RuntimeEffect::WaitFree, Feature::Web,
                       Result::Int},
        RuntimeBuiltin{"http_body", "fminus_web_http_body", {Param::Message}, 1, RuntimeEffect::WaitFree, Feature::Web,
                       Result::Bytes},
        RuntimeBuiltin{"http_progress", "fminus_web_http_progress", {Param::Response}, 1, RuntimeEffect::WaitFree,
                       Feature::Web, Result::Int},
        RuntimeBuiltin{"http_cancel", "fminus_web_http_cancel", {Param::Response}, 1, RuntimeEffect::Allocates,
                       Feature::Web, Result::Void},
        RuntimeBuiltin{"http_close", "fminus_web_http_close", {Param::Closable}, 1, RuntimeEffect::Allocates,
                       Feature::Web, Result::Void},
        // The web library's server (src/web/Web.h), in a build with FMINUS_WEB_SERVER too. http_listen() starts the
        // program's server where its host's policy says, with the table of every route literal the program has
        // (WebServer.cpp), and gives it, an http_server, at once: it allocates, as http_close of it does (the client's,
        // above, which closes a server too). http_port(server) is its port or why it has none, http_next(server) the
        // oldest request waiting (an http_request) or null, and http_accept(server, timeout_ms) the same after waiting
        // for one: it blocks. On a
        // request: http_is(r, "METHOD /path/%d") whether it is the route, a literal, which keeps the numbers its %d
        // took for http_arg(r, i); http_query_float(r, "name", fallback) and http_query_int a number of its query, the
        // name a literal; http_size and http_body (the client's, above) its body; and the replies, http_reply(r,
        // status), http_reply_bytes(r, status, data, bytes, "content/type") and http_reply_text(r, status,
        // "content/type", "format", ...), whose format's values follow it. Everything on a request, and http_port and
        // http_next, is wait-free.
        RuntimeBuiltin{"http_listen", "fminus_web_http_listen", {}, 0, RuntimeEffect::Allocates, Feature::WebServer,
                       Result::Server},
        RuntimeBuiltin{"http_port", "fminus_web_http_port", {Param::Server}, 1, RuntimeEffect::WaitFree,
                       Feature::WebServer, Result::Int},
        RuntimeBuiltin{"http_next", "fminus_web_http_next", {Param::Server}, 1, RuntimeEffect::WaitFree,
                       Feature::WebServer, Result::Request},
        RuntimeBuiltin{"http_accept", "fminus_web_http_accept", {Param::Server, Param::Int}, 2, RuntimeEffect::Blocks,
                       Feature::WebServer, Result::Request},
        RuntimeBuiltin{"http_is", "fminus_web_http_is", {Param::Request, Param::Route}, 2, RuntimeEffect::WaitFree,
                       Feature::WebServer, Result::Bool},
        RuntimeBuiltin{"http_arg", "fminus_web_http_arg", {Param::Request, Param::Int}, 2, RuntimeEffect::WaitFree,
                       Feature::WebServer, Result::Int},
        RuntimeBuiltin{"http_query_float", "fminus_web_http_query_float",
                       {Param::Request, Param::QueryName, Param::Float}, 3, RuntimeEffect::WaitFree, Feature::WebServer,
                       Result::Float},
        RuntimeBuiltin{"http_query_int", "fminus_web_http_query_int", {Param::Request, Param::QueryName, Param::Int}, 3,
                       RuntimeEffect::WaitFree, Feature::WebServer, Result::Int},
        RuntimeBuiltin{"http_reply", "fminus_web_http_reply", {Param::Request, Param::Int}, 2, RuntimeEffect::WaitFree,
                       Feature::WebServer, Result::Int},
        RuntimeBuiltin{"http_reply_bytes", "fminus_web_http_reply_bytes",
                       {Param::Request, Param::Int, Param::Bytes, Param::Int, Param::ContentType}, 5,
                       RuntimeEffect::WaitFree, Feature::WebServer, Result::Int},
        RuntimeBuiltin{"http_reply_text", "fminus_web_http_reply_text",
                       {Param::Request, Param::Int, Param::ContentType, Param::ReplyFormat}, 4, RuntimeEffect::WaitFree,
                       Feature::WebServer, Result::Int},
        // The file interface (runtime/Files.h, Runtime.h's fminus_rt_file_*), in a build with FMINUS_FILES. file_read(
        // "folder/name.ext", ...) opens the file the name says under the program's file context (Files.cpp) and reads
        // it whole into memory, and gives a handle, a file_content, which says why when it could not (a shared handle:
        // never null); file_list("folder/*.ext", ...) gives a file_listing, a snapshot of a folder's files whose names
        // fit, in their names' order, and file_read_nth(list, i) reads the i-th; file_close frees either. Each touches
        // the disk, waits on it and allocates: never on the audio thread. The name a literal with places for the ints
        // after every other argument, as a URL's. file_status, file_size, file_data (a const void*: the bytes, until
        // the file is closed) and file_count read a handle's fields, which never change once it was given: wait-free.
        RuntimeBuiltin{"file_read", "fminus_rt_file_read", {Param::FileName}, 1, RuntimeEffect::Allocates,
                       Feature::Files, Result::FileContent},
        RuntimeBuiltin{"file_status", "fminus_rt_file_status", {Param::FileHandle}, 1, RuntimeEffect::WaitFree,
                       Feature::Files, Result::Int},
        RuntimeBuiltin{"file_size", "fminus_rt_file_size", {Param::FileContent}, 1, RuntimeEffect::WaitFree,
                       Feature::Files, Result::Int},
        RuntimeBuiltin{"file_data", "fminus_rt_file_data", {Param::FileContent}, 1, RuntimeEffect::WaitFree,
                       Feature::Files, Result::Bytes},
        RuntimeBuiltin{"file_list", "fminus_rt_file_list", {Param::FilePattern}, 1, RuntimeEffect::Allocates,
                       Feature::Files, Result::FileListing},
        RuntimeBuiltin{"file_count", "fminus_rt_file_count", {Param::FileListing}, 1, RuntimeEffect::WaitFree,
                       Feature::Files, Result::Int},
        RuntimeBuiltin{"file_read_nth", "fminus_rt_file_read_nth", {Param::FileListing, Param::Int}, 2,
                       RuntimeEffect::Allocates, Feature::Files, Result::FileContent},
        RuntimeBuiltin{"file_close", "fminus_rt_file_close", {Param::FileHandle}, 1, RuntimeEffect::Allocates,
                       Feature::Files, Result::Void},
        // Its writing, in a build with FMINUS_FILES_WRITE too: file_write("folder/name.ext", data, bytes, ...) writes
        // the bytes as the whole file, replacing it whole or not at all, and file_append adds them at its end; each
        // gives a status (FILE_OK, or why not).
        RuntimeBuiltin{"file_write", "fminus_rt_file_write", {Param::FileName, Param::Bytes, Param::Int}, 3,
                       RuntimeEffect::Allocates, Feature::FilesWrite, Result::Int},
        RuntimeBuiltin{"file_append", "fminus_rt_file_append", {Param::FileName, Param::Bytes, Param::Int}, 3,
                       RuntimeEffect::Allocates, Feature::FilesWrite, Result::Int},
        // The string type's (Text.cpp), in every build, every one wait-free: a string is a pointer and a length over
        // bytes something else owns, and nothing here allocates on its behalf. Those without a symbol are made inline:
        // string_size(s) its length; string_byte(s, i) byte i as 0 to 255, or -1 outside it; string_slice(s, from, to)
        // the bytes from `from` up to `to`, each clamped to the string, empty when they cross; string_starts(s, with)
        // and string_ends(s, with); string_data(s) its bytes' address, a const void*; and string_of(data, size) a view
        // of `size` bytes at `data`, or the empty string for a null `data` or a negative size. The runtime's
        // (runtime/Text.cpp): string_find(s, what) where `what` first occurs, or -1 (an empty `what` at 0);
        // string_float(s, fallback) and string_int(s, fallback) the whole string read as a number as a query's is, or
        // the fallback; string_copy(out, cap, s) its first `cap` bytes copied, and its length.
        RuntimeBuiltin{"string_size", "", {Param::Text}, 1, RuntimeEffect::WaitFree, Feature::None, Result::Int},
        RuntimeBuiltin{"string_byte", "", {Param::Text, Param::Int}, 2, RuntimeEffect::WaitFree, Feature::None,
                       Result::Int},
        RuntimeBuiltin{"string_slice", "", {Param::Text, Param::Int, Param::Int}, 3, RuntimeEffect::WaitFree,
                       Feature::None, Result::String},
        RuntimeBuiltin{"string_find", "fminus_rt_string_find", {Param::Text, Param::Text}, 2, RuntimeEffect::WaitFree,
                       Feature::None, Result::Int},
        RuntimeBuiltin{"string_starts", "", {Param::Text, Param::Text}, 2, RuntimeEffect::WaitFree, Feature::None,
                       Result::Bool},
        RuntimeBuiltin{"string_ends", "", {Param::Text, Param::Text}, 2, RuntimeEffect::WaitFree, Feature::None,
                       Result::Bool},
        RuntimeBuiltin{"string_float", "fminus_rt_string_float", {Param::Text, Param::Float}, 2,
                       RuntimeEffect::WaitFree, Feature::None, Result::Float},
        RuntimeBuiltin{"string_int", "fminus_rt_string_int", {Param::Text, Param::Int}, 2, RuntimeEffect::WaitFree,
                       Feature::None, Result::Int},
        RuntimeBuiltin{"string_data", "", {Param::Text}, 1, RuntimeEffect::WaitFree, Feature::None, Result::Bytes},
        RuntimeBuiltin{"string_copy", "fminus_rt_string_copy", {Param::Chars, Param::Int, Param::Text}, 3,
                       RuntimeEffect::WaitFree, Feature::None, Result::Int},
        RuntimeBuiltin{"string_of", "", {Param::CharData, Param::Int}, 2, RuntimeEffect::WaitFree, Feature::None,
                       Result::String},
    };

    std::optional<ast::Type> paramType(Param param)
    {
        switch (param)
        {
        case Param::Int:
            return ast::Type::Int;
        case Param::Bytes:
            return ast::Type::pointerTo(ast::Kind::Void, true);
        case Param::Response:
        case Param::Message:
        case Param::Closable:
            return ast::Type::handleType(ast::Handle::HttpResponse);
        case Param::Request:
            return ast::Type::handleType(ast::Handle::HttpRequest);
        case Param::Server:
            return ast::Type::handleType(ast::Handle::HttpServer);
        case Param::JsonDoc:
        case Param::JsonRead:
            return ast::Type::handleType(ast::Handle::JsonDoc);
        case Param::XmlDoc:
        case Param::XmlRead:
            return ast::Type::handleType(ast::Handle::XmlDoc);
        case Param::FileContent:
        case Param::FileHandle:
            return ast::Type::handleType(ast::Handle::FileContent);
        case Param::FileListing:
            return ast::Type::handleType(ast::Handle::FileListing);
        case Param::Floats:
            return ast::Type::pointerTo(ast::Kind::Float, false);
        case Param::Float:
            return ast::Type::Float;
        case Param::Double:
            return ast::Type::Double;
        case Param::Bool:
            return ast::Type::Bool;
        case Param::Text:
            return ast::Type::String;
        case Param::Chars:
            return ast::Type::pointerTo(ast::Kind::Char, false);
        case Param::CharData:
            return ast::Type::pointerTo(ast::Kind::Char, true);
        case Param::String:
        case Param::Format:
        case Param::Path:
        case Param::Choices:
        case Param::XmlPath:
        case Param::Url:
        case Param::ContentType:
        case Param::Route:
        case Param::QueryName:
        case Param::ReplyFormat:
        case Param::FileName:
        case Param::FilePattern:
            break;
        }
        return std::nullopt;
    }

    std::optional<ast::Type> otherType(Param param)
    {
        switch (param)
        {
        case Param::Message:
            return ast::Type::handleType(ast::Handle::HttpRequest);
        case Param::Closable:
            return ast::Type::handleType(ast::Handle::HttpServer);
        case Param::FileHandle:
            return ast::Type::handleType(ast::Handle::FileListing);
        case Param::Int:
        case Param::Bytes:
        case Param::Floats:
        case Param::String:
        case Param::Format:
        case Param::Response:
        case Param::Request:
        case Param::Server:
        case Param::JsonDoc:
        case Param::JsonRead:
        case Param::XmlDoc:
        case Param::XmlRead:
        case Param::FileContent:
        case Param::FileListing:
        case Param::Float:
        case Param::Double:
        case Param::Bool:
        case Param::Path:
        case Param::Choices:
        case Param::XmlPath:
        case Param::Url:
        case Param::ContentType:
        case Param::Route:
        case Param::QueryName:
        case Param::ReplyFormat:
        case Param::FileName:
        case Param::FilePattern:
        case Param::Text:
        case Param::Chars:
        case Param::CharData:
            break;
        }
        return std::nullopt;
    }

    ast::Type resultType(Result result)
    {
        switch (result)
        {
        case Result::Int:
            return ast::Type::Int;
        case Result::Float:
            return ast::Type::Float;
        case Result::Double:
            return ast::Type::Double;
        case Result::Bool:
            return ast::Type::Bool;
        case Result::Response:
            return ast::Type::handleType(ast::Handle::HttpResponse);
        case Result::Request:
            return ast::Type::handleType(ast::Handle::HttpRequest);
        case Result::Server:
            return ast::Type::handleType(ast::Handle::HttpServer);
        case Result::JsonDoc:
            return ast::Type::handleType(ast::Handle::JsonDoc);
        case Result::XmlDoc:
            return ast::Type::handleType(ast::Handle::XmlDoc);
        case Result::FileContent:
            return ast::Type::handleType(ast::Handle::FileContent);
        case Result::FileListing:
            return ast::Type::handleType(ast::Handle::FileListing);
        case Result::Bytes:
            return ast::Type::pointerTo(ast::Kind::Void, true);
        case Result::String:
            return ast::Type::String;
        case Result::Void:
            break;
        }
        return ast::Type::Void;
    }

    bool isBuilt(Feature feature)
    {
        switch (feature)
        {
        case Feature::None:
            return true;
        case Feature::Json:
            return FMINUS_HAS_JSON != 0;
        case Feature::Xml:
            return FMINUS_HAS_XML != 0;
        case Feature::Web:
            return FMINUS_HAS_WEB != 0;
        case Feature::WebServer:
            return FMINUS_HAS_WEB_SERVER != 0;
        case Feature::Files:
            return FMINUS_HAS_FILES != 0;
        case Feature::FilesWrite:
            return FMINUS_HAS_FILES_WRITE != 0;
        }
        return false;
    }

    std::string notBuilt(std::string_view name, Feature feature)
    {
        // Writing is part of the file interface: a build without the interface lacks that first.
        if (feature == Feature::Files || (feature == Feature::FilesWrite && FMINUS_HAS_FILES == 0))
            return quoted(name) + " needs the file interface, which this build of FMinus leaves out (FMINUS_FILES=OFF)";
        if (feature == Feature::FilesWrite)
            return quoted(name) + " needs file writing, which this build of FMinus leaves out (FMINUS_FILES_WRITE=OFF)";
        const char* what = feature == Feature::Json        ? "the JSON parser"
                           : feature == Feature::Xml       ? "the XML parser"
                           : feature == Feature::WebServer ? "the web server"
                                                           : "the web library";
        const char* option = feature == Feature::Json        ? "FMINUS_JSON"
                             : feature == Feature::Xml       ? "FMINUS_XML"
                             : feature == Feature::WebServer ? "FMINUS_WEB_SERVER"
                                                             : "FMINUS_WEB";
        return quoted(name) + " needs " + what + ", which this build of FMinus leaves out (" + option + "=OFF)";
    }

    namespace
    {
        // What a runtime function the builtins call does with memory (describeRuntime gives its declaration the
        // attributes that say so).
        enum class RuntimeKind
        {
            Allocation,    // fminus_rt_malloc and fminus_rt_calloc (runtime/Runtime.cpp): a new block, or null
            Release,       // fminus_rt_free: C's free
            Plan,          // fminus_rt_fft_create, fminus_rt_fft_create_complex (runtime/Fft.cpp): a new plan, or null
            PlanRelease,   // fminus_rt_fft_destroy
            Transform,     // fminus_rt_fft_forward_real and the others: reads one buffer, writes another
            Parallel,      // fminus_rt_parallel_for (runtime/Parallel.cpp): runs a region's body, which does anything
            Decode,        // fminus_rt_bytes_floats, fminus_rt_bytes_pcm16 (runtime/Bytes.cpp): reads one, writes one
            ReadText,      // fminus_rt_string_hash: reads a literal and its ints
            Document,      // fminus_rt_json_parse, fminus_rt_xml_parse (runtime/Json.cpp, Xml.cpp): reads a text, gives
                           // a new document, or null
            DocumentRelease,   // fminus_rt_json_free, fminus_rt_xml_free
            Read,          // fminus_rt_json_float, fminus_rt_xml_float and the other reads: a document, a path and its
                           // ints
            ReadChoice,    // fminus_rt_json_choice, fminus_rt_xml_choice: and the literals' table
            ReadInto,      // fminus_rt_json_floats, fminus_rt_json_base64_floats, fminus_rt_xml_floats: and writes
                           // floats
            WebStart,      // fminus_web_http_get, fminus_web_http_post (src/web/Web.h): reads a URL literal, its ints
                           // (and a POST's body and content type), gives a new request
            WebWait,       // fminus_web_http_wait: waits on a request
            WebRead,       // fminus_web_http_status, _size, _body, _progress: a request's atomics, wait-free
            WebEnd,        // fminus_web_http_cancel, fminus_web_http_close: stops a request (and frees it)
            ServerListen,  // fminus_web_http_listen: reads the route table, gives the context's server
            ServerTake,    // fminus_web_http_next, _next_in: claims a request of the server's (of a route set's),
                           // wait-free
            ServerWait,    // fminus_web_http_accept, _accept_in: waits for one
            ServerRoute,   // fminus_web_http_route: which route a request was published for, its places kept in it
            ServerMatch,   // fminus_web_http_is: reads a route literal, keeps the numbers it took in the request
            ServerQuery,   // fminus_web_http_query_float, _int: reads a name literal and the request
            ServerText,    // fminus_web_http_query_text: the same, giving the value's bytes and their count
            ServerReply,   // fminus_web_http_reply, _reply_bytes, _reply_text: answers, and hands the request back
            FileOpen,      // fminus_rt_file_read, fminus_rt_file_list (runtime/Files.h): reads a name literal and its
                           // ints, gives a new handle (or a shared one that says why)
            FileNth,       // fminus_rt_file_read_nth: reads a listing, gives a new handle
            FileRead,      // fminus_rt_file_status, _size, _data, _count: a handle's fields, which never change
            FileClose,     // fminus_rt_file_close: frees a handle, and a file's bytes, which the program reads
            FileWrite,     // fminus_rt_file_write, fminus_rt_file_append: reads a name literal, its ints and the bytes
            TextRead,      // fminus_rt_string_equal, _find, _float, _int (runtime/Text.cpp): reads strings' bytes
            TextCopy,      // fminus_rt_string_copy: reads a string's bytes, writes them into a char array
        };

        struct RuntimeFacts
        {
            std::string_view symbol;
            RuntimeKind kind;
        };

        constexpr std::array kRuntimeFacts{
            RuntimeFacts{"fminus_rt_malloc", RuntimeKind::Allocation},
            RuntimeFacts{"fminus_rt_calloc", RuntimeKind::Allocation},
            RuntimeFacts{"fminus_rt_free", RuntimeKind::Release},
            RuntimeFacts{"fminus_rt_fft_create", RuntimeKind::Plan},
            RuntimeFacts{"fminus_rt_fft_create_complex", RuntimeKind::Plan},
            RuntimeFacts{"fminus_rt_fft_destroy", RuntimeKind::PlanRelease},
            RuntimeFacts{"fminus_rt_fft_forward_real", RuntimeKind::Transform},
            RuntimeFacts{"fminus_rt_fft_inverse_real", RuntimeKind::Transform},
            RuntimeFacts{"fminus_rt_fft_forward_complex", RuntimeKind::Transform},
            RuntimeFacts{"fminus_rt_fft_inverse_complex", RuntimeKind::Transform},
            RuntimeFacts{"fminus_rt_parallel_for", RuntimeKind::Parallel},
            RuntimeFacts{"fminus_rt_bytes_floats", RuntimeKind::Decode},
            RuntimeFacts{"fminus_rt_bytes_pcm16", RuntimeKind::Decode},
            RuntimeFacts{"fminus_rt_string_hash", RuntimeKind::ReadText},
            RuntimeFacts{"fminus_rt_json_parse", RuntimeKind::Document},
            RuntimeFacts{"fminus_rt_json_free", RuntimeKind::DocumentRelease},
            RuntimeFacts{"fminus_rt_json_float", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_json_double", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_json_int", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_json_bool", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_json_count", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_json_choice", RuntimeKind::ReadChoice},
            RuntimeFacts{"fminus_rt_json_floats", RuntimeKind::ReadInto},
            RuntimeFacts{"fminus_rt_json_base64_floats", RuntimeKind::ReadInto},
            RuntimeFacts{"fminus_rt_xml_parse", RuntimeKind::Document},
            RuntimeFacts{"fminus_rt_xml_free", RuntimeKind::DocumentRelease},
            RuntimeFacts{"fminus_rt_xml_float", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_xml_double", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_xml_int", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_xml_bool", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_xml_count", RuntimeKind::Read},
            RuntimeFacts{"fminus_rt_xml_choice", RuntimeKind::ReadChoice},
            RuntimeFacts{"fminus_rt_xml_floats", RuntimeKind::ReadInto},
            RuntimeFacts{"fminus_web_http_get", RuntimeKind::WebStart},
            RuntimeFacts{"fminus_web_http_post", RuntimeKind::WebStart},
            RuntimeFacts{"fminus_web_http_wait", RuntimeKind::WebWait},
            RuntimeFacts{"fminus_web_http_status", RuntimeKind::WebRead},
            RuntimeFacts{"fminus_web_http_size", RuntimeKind::WebRead},
            RuntimeFacts{"fminus_web_http_body", RuntimeKind::WebRead},
            RuntimeFacts{"fminus_web_http_progress", RuntimeKind::WebRead},
            RuntimeFacts{"fminus_web_http_cancel", RuntimeKind::WebEnd},
            RuntimeFacts{"fminus_web_http_close", RuntimeKind::WebEnd},
            RuntimeFacts{"fminus_web_http_listen", RuntimeKind::ServerListen},
            RuntimeFacts{"fminus_web_http_port", RuntimeKind::WebRead},
            RuntimeFacts{"fminus_web_http_next", RuntimeKind::ServerTake},
            RuntimeFacts{"fminus_web_http_accept", RuntimeKind::ServerWait},
            RuntimeFacts{"fminus_web_http_next_in", RuntimeKind::ServerTake},
            RuntimeFacts{"fminus_web_http_accept_in", RuntimeKind::ServerWait},
            RuntimeFacts{"fminus_web_http_route", RuntimeKind::ServerRoute},
            RuntimeFacts{"fminus_web_http_is", RuntimeKind::ServerMatch},
            RuntimeFacts{"fminus_web_http_arg", RuntimeKind::WebRead},
            RuntimeFacts{"fminus_web_http_query_float", RuntimeKind::ServerQuery},
            RuntimeFacts{"fminus_web_http_query_int", RuntimeKind::ServerQuery},
            RuntimeFacts{"fminus_web_http_query_text", RuntimeKind::ServerText},
            RuntimeFacts{"fminus_web_http_reply", RuntimeKind::ServerReply},
            RuntimeFacts{"fminus_web_http_reply_bytes", RuntimeKind::ServerReply},
            RuntimeFacts{"fminus_web_http_reply_text", RuntimeKind::ServerReply},
            RuntimeFacts{"fminus_rt_file_read", RuntimeKind::FileOpen},
            RuntimeFacts{"fminus_rt_file_list", RuntimeKind::FileOpen},
            RuntimeFacts{"fminus_rt_file_read_nth", RuntimeKind::FileNth},
            RuntimeFacts{"fminus_rt_file_status", RuntimeKind::FileRead},
            RuntimeFacts{"fminus_rt_file_size", RuntimeKind::FileRead},
            RuntimeFacts{"fminus_rt_file_data", RuntimeKind::FileRead},
            RuntimeFacts{"fminus_rt_file_count", RuntimeKind::FileRead},
            RuntimeFacts{"fminus_rt_file_close", RuntimeKind::FileClose},
            RuntimeFacts{"fminus_rt_file_write", RuntimeKind::FileWrite},
            RuntimeFacts{"fminus_rt_file_append", RuntimeKind::FileWrite},
            RuntimeFacts{"fminus_rt_string_equal", RuntimeKind::TextRead},
            RuntimeFacts{"fminus_rt_string_find", RuntimeKind::TextRead},
            RuntimeFacts{"fminus_rt_string_float", RuntimeKind::TextRead},
            RuntimeFacts{"fminus_rt_string_int", RuntimeKind::TextRead},
            RuntimeFacts{"fminus_rt_string_copy", RuntimeKind::TextCopy},
        };

        // The heap's allocator family, named after its first function as LangRef asks: LLVM pairs an allocation with
        // a free only within one family.
        constexpr std::string_view kHeapFamily = "fminus_rt_malloc";

        // Gives the runtime's function `function` (kRuntimeFacts) the attributes that are true of its code, so LLVM
        // knows a call of it touches only what it touches: a call of fft_forward then changes no module's fields, and
        // memory from calloc is fresh and zeroed. Each one must stay true of runtime/Runtime.cpp, runtime/Fft.cpp,
        // runtime/Bytes.cpp, runtime/Json.cpp, runtime/Xml.cpp, runtime/Files.cpp, web/Web.cpp and web/Server.cpp:
        // an attribute that is not is a miscompile. Every one of them is noexcept (nounwind), and all but
        // fminus_rt_parallel_for and the web library's that wait for another thread (http_wait, http_cancel,
        // http_close and http_accept) come back (willreturn): it runs a region's body, which may loop for ever, as a
        // loop with a constant condition may, and http_accept(server, -1) waits for a request that may never come. None takes nonnull: a null plan is a plan that does nothing, and the heap's
        // results may be null. Nor does an FFT buffer take noalias, though `out` must not overlap `in`: a program that
        // transforms in place gets what PFFFT gives it, as before.
        void describeRuntime(llvm::Function& function)
        {
            const RuntimeFacts* facts = nullptr;
            for (const RuntimeFacts& candidate : kRuntimeFacts)
            {
                if (candidate.symbol == std::string_view(function.getName()))
                    facts = &candidate;
            }
            if (facts == nullptr)
                return;
            llvm::LLVMContext& context = function.getContext();
            const llvm::Attribute keepsNone = llvm::Attribute::getWithCaptureInfo(context, llvm::CaptureInfo::none());
            function.setDoesNotThrow();
            if (facts->kind != RuntimeKind::Parallel && facts->kind != RuntimeKind::WebWait
                && facts->kind != RuntimeKind::WebEnd && facts->kind != RuntimeKind::ServerWait)
                function.setWillReturn();
            switch (facts->kind)
            {
            case RuntimeKind::Allocation:
                // count * size bytes (malloc's: size), at least, aligned to 64 and zeroed, or null: C's allocator, as
                // LLVM declares calloc (allockind, allocsize, a noalias result), zeroed for malloc too. So LLVM knows a
                // block is fresh and reads 0 until written, and may remove one the program never uses, as it may C's
                // (LangRef's allocation elision: the block comes from a "virtual" allocator, which does not fail).
                // Beyond the new block, it changes only the system allocator's state and errno, which no FMinus code
                // can read.
                function.addFnAttr(
                    llvm::Attribute::getWithAllocKind(context, llvm::AllocFnKind::Alloc | llvm::AllocFnKind::Zeroed));
                function.addFnAttr(llvm::Attribute::getWithAllocSizeArgs(
                    context, 0, function.arg_size() == 2 ? std::optional<unsigned>(1) : std::nullopt));
                function.addFnAttr("alloc-family", kHeapFamily);
                function.setMemoryEffects(
                    llvm::MemoryEffects::inaccessibleOrErrnoMemOnly(llvm::ModRefInfo::ModRef, llvm::ModRefInfo::Mod));
                function.addRetAttr(llvm::Attribute::NoAlias);
                function.addRetAttr(llvm::Attribute::NoUndef);
                function.addRetAttr(llvm::Attribute::getWithAlignment(context, llvm::Align(64)));
                break;
            case RuntimeKind::Release:
                // C's free of a block of the same family (or of null, which does nothing). It keeps no pointer.
                function.addFnAttr(llvm::Attribute::getWithAllocKind(context, llvm::AllocFnKind::Free));
                function.addFnAttr("alloc-family", kHeapFamily);
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.addParamAttr(0, llvm::Attribute::AllocatedPointer);
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::Plan:
                // A new plan, or null. It allocates, but it is no allocator LLVM may remove: its null says that PFFFT
                // takes no plan of the size, every time, and a removed call would be taken for a plan made. What it
                // changes, beyond the new plan, is the system allocator's state and errno.
                function.setMemoryEffects(
                    llvm::MemoryEffects::inaccessibleOrErrnoMemOnly(llvm::ModRefInfo::ModRef, llvm::ModRefInfo::Mod));
                function.addRetAttr(llvm::Attribute::NoAlias);
                function.addRetAttr(llvm::Attribute::NoUndef);
                break;
            case RuntimeKind::PlanRelease:
                // Reads the plan and frees it, and the plan's own memory. It keeps no pointer.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::Transform:
                // Reads the plan and `in` and writes `out` (where it reads only what it wrote), keeping no pointer to
                // any of them. Beyond them it touches only the plan's own memory, PFFFT's setup and the plan's three
                // buffers, which only the plan points to: inaccessible memory, since a plan is opaque, an fft_plan that
                // a program hands to the FFT's builtins and reads nothing through. (One that converted it to a module
                // pointer and read the runtime's struct through that would read an object as a type it is not, which
                // C leaves undefined.) It allocates, frees and waits for nothing (the effects let it run on the audio
                // thread), so it is nofree and nosync too.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.setDoesNotFreeMemory();
                function.setNoSync();
                function.addParamAttr(0, llvm::Attribute::ReadOnly);
                function.addParamAttr(1, llvm::Attribute::ReadOnly);
                function.addParamAttr(2, llvm::Attribute::WriteOnly);
                for (unsigned arg = 0; arg < 3; ++arg)
                    function.addParamAttr(arg, keepsNone);
                break;
            case RuntimeKind::Parallel:
                // The body reads and writes what its context reaches, and calls what it calls: LLVM's default, any
                // memory, is what is true of it.
                break;
            case RuntimeKind::Decode:
                // Reads `data` and writes `out` (no more than `cap` floats of it), keeping no pointer to either, and
                // touches nothing else: no memory of its own, no allocation, no lock. `data` may be null and `out`
                // too, so neither is nonnull; and the two may overlap (a program may decode in place), so neither is
                // noalias. readonly and writeonly stay true even then.
                function.setMemoryEffects(llvm::MemoryEffects::argMemOnly());
                function.setDoesNotFreeMemory();
                function.setNoSync();
                function.addParamAttr(0, llvm::Attribute::ReadOnly);
                function.addParamAttr(0, keepsNone);
                function.addParamAttr(2, llvm::Attribute::WriteOnly);
                function.addParamAttr(2, keepsNone);
                break;
            case RuntimeKind::ReadText:
                // Reads the literal and the ints, keeping no pointer to either, and nothing else.
                function.setMemoryEffects(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref));
                function.setDoesNotFreeMemory();
                function.setNoSync();
                for (unsigned arg : {0u, 2u})
                {
                    function.addParamAttr(arg, llvm::Attribute::ReadOnly);
                    function.addParamAttr(arg, keepsNone);
                }
                break;
            case RuntimeKind::Document:
                // Reads the text, keeping no pointer to it (the document holds a copy), and gives a new document, or
                // null, as fminus_rt_fft_create gives a plan: no allocator LLVM may remove, since its null says the
                // text is no JSON (or no XML). Beyond the text and the new document, it changes the system
                // allocator's state and errno.
                function.setMemoryEffects(
                    llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)
                    | llvm::MemoryEffects::inaccessibleOrErrnoMemOnly(llvm::ModRefInfo::ModRef, llvm::ModRefInfo::Mod));
                function.addParamAttr(0, llvm::Attribute::ReadOnly);
                function.addParamAttr(0, keepsNone);
                function.addRetAttr(llvm::Attribute::NoAlias);
                function.addRetAttr(llvm::Attribute::NoUndef);
                break;
            case RuntimeKind::DocumentRelease:
                // Frees the document and the memory only it points to. It keeps no pointer.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::Read:
            case RuntimeKind::ReadChoice:
            case RuntimeKind::ReadInto:
                // Reads the document, the path's table (a constant of the program's, one block: Paths.cpp), the ints
                // and json_choice's or xml_choice's table, keeping no pointer to any of them, and writes only
                // json_floats', json_base64_floats' and xml_floats' `out` (no more than `cap` floats of it).
                // A document is opaque, as an FFT plan is: what it points to, its values and its copy of the text, only
                // it points to, so a read touches inaccessible memory there, and writes none. It allocates, frees and
                // waits for nothing (the effects let it run on the audio thread): nofree and nosync too.
                function.setMemoryEffects(
                    llvm::MemoryEffects::argMemOnly(facts->kind == RuntimeKind::ReadInto ? llvm::ModRefInfo::ModRef
                                                                                         : llvm::ModRefInfo::Ref)
                    | llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref));
                function.setDoesNotFreeMemory();
                function.setNoSync();
                for (unsigned arg : {0u, 1u, 2u})
                {
                    function.addParamAttr(arg, llvm::Attribute::ReadOnly);
                    function.addParamAttr(arg, keepsNone);
                }
                if (facts->kind != RuntimeKind::Read)
                {
                    function.addParamAttr(4, facts->kind == RuntimeKind::ReadInto ? llvm::Attribute::WriteOnly
                                                                                   : llvm::Attribute::ReadOnly);
                    function.addParamAttr(4, keepsNone);
                }
                break;
            case RuntimeKind::WebStart:
                // Reads the URL literal and its ints, and a POST's body and content type, keeping no pointer to any of
                // them (the library copies what it keeps), and changes the context, the request it makes and the
                // library's own state, which only the context's pointer reaches: the context is opaque, as an FFT plan
                // is (the request keeps the context's pointer, so that one is captured). Beyond them, the system
                // allocator's state and errno. Its result may be the library's one shared refused request (Web.h):
                // no noalias.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly()
                                          | llvm::MemoryEffects::errnoMemOnly(llvm::ModRefInfo::Mod));
                for (unsigned arg = 1; arg < function.arg_size(); ++arg)
                {
                    if (!function.getArg(arg)->getType()->isPointerTy())
                        continue;
                    function.addParamAttr(arg, llvm::Attribute::ReadOnly);
                    function.addParamAttr(arg, keepsNone);
                }
                function.addRetAttr(llvm::Attribute::NoUndef);
                break;
            case RuntimeKind::WebWait:
            case RuntimeKind::WebEnd:
                // A lock and a condition variable of the request's context, the I/O thread's queue, and a close's
                // free: the request's memory and the library's, which only its handle reaches. It keeps no pointer.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::WebRead:
                // Acquire loads of the request's atomics, which the library's I/O thread writes: not readonly, so that
                // LLVM neither merges two calls nor moves one out of a polling loop (an acquire load orders, and
                // synchronizes with the thread that published: no nosync either). It frees nothing and keeps no
                // pointer.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.setDoesNotFreeMemory();
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::ServerListen:
                // Binds and listens under the context's mutex, makes the server (the library's memory, which only the
                // context's pointer reaches, as WebStart's request) and copies the route table: it reads the table and
                // the lengths, keeping no pointer to either, and the literals the table points to, which no argument
                // points to (other memory, read only: the program's constants). Beyond them, the system allocator's
                // state, the sockets, and errno, which it reads after a failed bind (web/SocketsPosix.cpp). Its result
                // may be the library's one shared refused server, or the context's server again: no noalias.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly()
                                          | llvm::MemoryEffects::errnoMemOnly(llvm::ModRefInfo::ModRef)
                                          | llvm::MemoryEffects::otherMemOnly(llvm::ModRefInfo::Ref));
                for (unsigned arg : {1u, 2u})
                {
                    function.addParamAttr(arg, llvm::Attribute::ReadOnly);
                    function.addParamAttr(arg, keepsNone);
                }
                function.addRetAttr(llvm::Attribute::NoUndef);
                break;
            case RuntimeKind::ServerTake:
                // Acquire loads of the server's slots and a compare-and-swap of one, which the I/O thread and every
                // other taker race with: it changes what the next call reads (two calls are two requests, and a
                // polling loop must call it each time round), so it writes. The slots are the library's memory. It
                // frees nothing, and gives a request, never the server's own pointer. _next_in reads its route set, a
                // constant of the program's, and keeps no pointer to it.
            case RuntimeKind::ServerWait:
                // The same, and the server's lock and condition variable, waited on (and the context's deadline
                // scopes, inaccessible too).
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.setDoesNotFreeMemory();
                function.addRetAttr(llvm::Attribute::NoUndef);
                if (function.arg_size() > 2)   // _next_in, _accept_in
                {
                    function.addParamAttr(1, llvm::Attribute::ReadOnly);
                    function.addParamAttr(1, keepsNone);
                }
                break;
            case RuntimeKind::ServerRoute:
                // Reads the request (an acquire load of its phase, then the route it was published for) and writes
                // that route's places into it, which http_arg reads: so it writes, as http_is does, and an http_arg
                // after it reads again. Keeps no pointer to it.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.setDoesNotFreeMemory();
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::ServerMatch:
                // Reads the request (an acquire load of its phase, which the I/O thread writes, then its method and
                // path) and the route literal, and, when it matches, writes the numbers it took into the request, which
                // http_arg reads: so it writes, and an http_arg after it reads again. Keeps no pointer to either.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.setDoesNotFreeMemory();
                function.addParamAttr(0, keepsNone);
                function.addParamAttr(1, llvm::Attribute::ReadOnly);
                function.addParamAttr(1, keepsNone);
                break;
            case RuntimeKind::ServerQuery:
            case RuntimeKind::ServerText:
                // As WebRead (an acquire load of the request's phase, then its query), and the name literal, which it
                // only reads. query_text writes the value's length (when its pointer is not null) and gives the
                // address of bytes in the request's slot, which the I/O thread reuses after the reply (whose memory is
                // any, so loads through it stay before it): neither noalias nor nonnull, null when there is none.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
                function.setDoesNotFreeMemory();
                function.addParamAttr(0, keepsNone);
                function.addParamAttr(1, llvm::Attribute::ReadOnly);
                function.addParamAttr(1, keepsNone);
                if (facts->kind == RuntimeKind::ServerText)
                {
                    function.addParamAttr(3, llvm::Attribute::WriteOnly);
                    function.addParamAttr(3, keepsNone);
                }
                break;
            case RuntimeKind::ServerReply:
                // Writes the reply into the request's own buffer and publishes it with a compare-and-swap, which hands
                // the request back: the I/O thread then sends it and reuses the request's memory, its body among it,
                // which the program reads through http_body's pointer. So a load of that body must not move after the
                // call, which LLVM knows only of a call that may write any memory: its memory is left as it is (any).
                // It reads the data, the content type, the format and the values, keeping no pointer to any, frees
                // nothing and keeps no pointer to the request.
                function.setDoesNotFreeMemory();
                function.addParamAttr(0, keepsNone);
                for (unsigned arg = 2; arg < function.arg_size(); ++arg)
                {
                    if (!function.getArg(arg)->getType()->isPointerTy())
                        continue;
                    function.addParamAttr(arg, llvm::Attribute::ReadOnly);
                    function.addParamAttr(arg, keepsNone);
                }
                break;
            case RuntimeKind::FileOpen:
            case RuntimeKind::FileWrite:
                // Reads the name literal and its ints (and file_write's and file_append's bytes), keeping no pointer to
                // any of them, and changes the context and what only it reaches: its folders, its quotas, its list of
                // the handles it gave (a new one linked in beside those the program holds, whose fields the reads read
                // never change: a handle is opaque, as an FFT plan is), the system allocator's state; it reads and
                // writes errno (Files.cpp turns a failed call's errno into a status). The handle keeps the context's
                // pointer, so that one is captured. It frees nothing the program reads: a file's bytes are freed by
                // file_close alone. Its result may be one of the runtime's shared handles of failures: no noalias.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly()
                                          | llvm::MemoryEffects::errnoMemOnly(llvm::ModRefInfo::ModRef));
                for (unsigned arg = 1; arg < function.arg_size(); ++arg)
                {
                    if (!function.getArg(arg)->getType()->isPointerTy())
                        continue;
                    function.addParamAttr(arg, llvm::Attribute::ReadOnly);
                    function.addParamAttr(arg, keepsNone);
                }
                if (facts->kind == RuntimeKind::FileOpen)
                    function.addRetAttr(llvm::Attribute::NoUndef);
                break;
            case RuntimeKind::FileNth:
                // Reads the listing, and through it its context (what only the context reaches, as FileOpen's), and
                // gives a new handle as file_read does: the same memory, with the listing for the literal. It keeps no
                // pointer to the listing.
                function.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly()
                                          | llvm::MemoryEffects::errnoMemOnly(llvm::ModRefInfo::ModRef));
                function.addParamAttr(0, keepsNone);
                function.addRetAttr(llvm::Attribute::NoUndef);
                break;
            case RuntimeKind::FileRead:
                // Plain loads of the handle's first fields, which never change once the handle was given (and a shared
                // failure's never at all): no lock, no atomic, no allocation, no system call. readonly, so that LLVM
                // merges two reads of one handle with nothing between that may write (a handle is opaque: what the
                // context's other functions change, inaccessible memory, is a write between). It keeps no pointer.
                function.setMemoryEffects(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)
                                          | llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::Ref));
                function.setDoesNotFreeMemory();
                function.setNoSync();
                function.addParamAttr(0, llvm::Attribute::ReadOnly);
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::FileClose:
                // Frees the handle, and a file's bytes, which the program reads through file_data's pointer: so a load
                // of them must not move after the call, which LLVM knows only of a call that may write any memory: its
                // memory is left as it is (any). It keeps no pointer to the handle.
                function.addParamAttr(0, keepsNone);
                break;
            case RuntimeKind::TextRead:
            case RuntimeKind::TextCopy:
                // Reads the bytes its strings point to (each a pointer and a length), and string_copy writes `out`
                // (no more than `cap` bytes of it, which may overlap the string: a memmove), keeping no pointer to any;
                // nothing else. It allocates, frees, locks and waits for nothing (the effects let it run on the audio
                // thread): nofree and nosync. A pointer may be null with a length of 0, so none is nonnull.
                function.setMemoryEffects(llvm::MemoryEffects::argMemOnly(
                    facts->kind == RuntimeKind::TextCopy ? llvm::ModRefInfo::ModRef : llvm::ModRefInfo::Ref));
                function.setDoesNotFreeMemory();
                function.setNoSync();
                for (unsigned arg = 0; arg < function.arg_size(); ++arg)
                {
                    if (!function.getArg(arg)->getType()->isPointerTy())
                        continue;
                    function.addParamAttr(arg, facts->kind == RuntimeKind::TextCopy && arg == 0
                                                   ? llvm::Attribute::WriteOnly
                                                   : llvm::Attribute::ReadOnly);
                    function.addParamAttr(arg, keepsNone);
                }
                break;
            }
        }
    } // namespace

    // The reductions of a float vector to one float: its sum (in lane order, so the result does not depend on
    // fast-math), its least and its greatest lane (NaN lanes ignored, as fminf and fmaxf ignore them).
    constexpr std::array<std::string_view, 3> kReductions{"reduce_add", "reduce_min", "reduce_max"};

    bool isReduction(std::string_view name)
    {
        for (std::string_view reduction : kReductions)
        {
            if (reduction == name)
                return true;
        }
        return false;
    }

    // poly_lanes(): how many lanes a poly float has, an int. poly_index(): a poly float whose lanes hold their own
    // indices, 0, 1, 2, ...: what tells the lanes apart.
    bool isPolyQuery(std::string_view name)
    {
        return name == "poly_lanes" || name == "poly_index";
    }

    // poly_load(p) and poly_store(p, v): a poly float from, or to, the poly_lanes() floats at p, lane i at p[i]. With a
    // poly bool after them, only the lanes it sets: a lane it doesn't set is neither read (it loads as 0) nor written,
    // so the last, partial step of a loop over a buffer stays inside it.
    bool isPolyMemory(std::string_view name)
    {
        return name == "poly_load" || name == "poly_store";
    }

    const Allocation* findAllocation(std::string_view name)
    {
        for (const Allocation& allocation : kAllocations)
        {
            if (allocation.name == name)
                return &allocation;
        }
        return nullptr;
    }

    const Fft* findFft(std::string_view name)
    {
        for (const Fft& fft : kFfts)
        {
            if (fft.name == name)
                return &fft;
        }
        return nullptr;
    }

    const RuntimeBuiltin* findRuntimeBuiltin(std::string_view name)
    {
        for (const RuntimeBuiltin& builtin : kRuntimeBuiltins)
        {
            if (builtin.name == name)
                return &builtin;
        }
        return nullptr;
    }

    // What the effects refuse a nonallocating function (Effects.cpp): a builtin that allocates or frees.
    bool allocates(std::string_view name)
    {
        const Fft* fft = findFft(name);
        const RuntimeBuiltin* runtime = findRuntimeBuiltin(name);
        return findAllocation(name) != nullptr || (fft != nullptr && fft->allocates)
               || (runtime != nullptr && runtime->effect == RuntimeEffect::Allocates);
    }

    // What the effects refuse a nonblocking function besides (Effects.cpp): a builtin that may wait.
    bool blocks(std::string_view name)
    {
        const RuntimeBuiltin* runtime = findRuntimeBuiltin(name);
        return runtime != nullptr && runtime->effect == RuntimeEffect::Blocks;
    }

    const Builtin* findBuiltin(std::string_view name)
    {
        for (const Builtin& builtin : kBuiltins)
        {
            if (builtin.name == name)
                return &builtin;
        }
        return nullptr;
    }

    Lowering::Value Lowering::lowerCall(SourceLocation location, const ast::CallExpr& call)
    {
        // The types the arguments are passed as, where the callee says before they are lowered: a method's
        // parameters (after `this`), a double builtin's doubles, a function's parameters. A numeric argument takes a
        // double's (lowerAs): f(0.1) of a double parameter passes C's 0.1.
        std::vector<ast::Type> passedAs;
        const ast::Module* module = function_->module;
        const Builtin* builtin = findBuiltin(call.callee);
        if (const auto method = module != nullptr ? functions_.find(module->name + "::" + call.callee) : functions_.end();
            method != functions_.end())
        {
            for (std::size_t i = 1; i < method->second->params.size(); ++i)
                passedAs.push_back(method->second->params[i].type);
        }
        else if (builtin != nullptr && builtin->type == ast::Kind::Double)
            passedAs.assign(builtin->arity, ast::Type::Double);
        else if (const auto function = functions_.find(call.callee); function != functions_.end())
        {
            for (const ast::Param& param : function->second->params)
                passedAs.push_back(param.type);
        }
        const bool isMethod = module != nullptr && functions_.contains(module->name + "::" + call.callee);
        if (const RuntimeBuiltin* runtime = findRuntimeBuiltin(call.callee); runtime != nullptr && !isMethod)
            return lowerRuntimeBuiltin(location, *runtime, call);
        std::vector<Value> args;
        for (std::size_t i = 0; i < call.args.size(); ++i)
        {
            const std::optional<ast::Type> type = i < passedAs.size() ? std::optional(passedAs[i]) : std::nullopt;
            if (isString(*call.args[i]))
            {
                // A math builtin's parameters are floats (or doubles), as its prototype in <math.h> says, and a size's,
                // a plan's length and hash's and randf's ints.
                const ast::Type scalar = builtin != nullptr && builtin->type == ast::Kind::Double ? ast::Type::Double
                                                                                                  : ast::Type::Float;
                const bool sized = (findAllocation(call.callee) != nullptr && call.callee != "free")
                                   || call.callee == "fft_create" || call.callee == "fft_create_complex"
                                   || isHash(call.callee);
                std::optional<ast::Type> to = type;
                if (!to && builtin != nullptr)
                    to = scalar;
                else if (!to && sized)
                    to = ast::Type::Int;
                args.push_back(refuseString(*call.args[i], to, Use::Passing, call.args[i]->location));
            }
            else
                args.push_back(type ? lowerAs(*call.args[i], *type) : lower(*call.args[i]));
        }

        if (isMethod)
            return callMethod(location, *module, self(), false, call.callee, args);
        if (isReduction(call.callee))
            return lowerReduction(location, call, args);
        if (isPolyQuery(call.callee))
        {
            if (!args.empty())
                return wrongArity(location, args.size(), 0);
            if (call.callee == "poly_lanes")
                return {builder_.getInt32(polyLanes_), ast::Type::Int};
            std::vector<llvm::Constant*> indices;
            for (unsigned i = 0; i < polyLanes_; ++i)
                indices.push_back(llvm::ConstantFP::get(builder_.getFloatTy(), static_cast<double>(i)));
            return {llvm::ConstantVector::get(indices), ast::Type::PolyFloat};
        }
        if (isPolyMemory(call.callee))
            return lowerPolyMemory(location, call, args);
        if (const Allocation* allocation = findAllocation(call.callee))
        {
            calls_[function_->name].push_back({location, call.callee});   // what Effects.cpp checks
            return lowerAllocation(location, *allocation, args);
        }
        if (const Fft* fft = findFft(call.callee))
        {
            if (fft->allocates)
                calls_[function_->name].push_back({location, call.callee});
            return lowerFft(location, *fft, args);
        }
        if (isHash(call.callee))
            return lowerHash(location, call, args);   // Hash.cpp
        std::vector<llvm::Value*> irArgs;
        if (builtin != nullptr)
        {
            if (args.size() != builtin->arity)
                return wrongArity(location, args.size(), builtin->arity);
            // Overloaded on its arguments, as OpenCL C's math functions are: with a vector argument it is the
            // vector intrinsic (llvm.fabs.v4f32), and scalar arguments beside it are broadcast. A double argument
            // converts to a float, as C converts it for a float parameter (sqrtf(d) rounds d first), and to no vector.
            // A double function takes doubles, which every scalar converts to, and no vector.
            const ast::Type scalar = builtin->type == ast::Kind::Double ? ast::Type::Double : ast::Type::Float;
            ast::Type type = scalar;
            for (const Value& arg : args)
            {
                if (arg.failed && !ast::isArithmetic(arg.type))
                    return recovery(scalar);   // its own error says why (Value)
                if (!ast::isArithmetic(arg.type) || (scalar == ast::Type::Double && ast::isVector(arg.type)))
                {
                    incompatible(location, Use::Passing, arg.type, scalar);
                    return poison(scalar);
                }
                if (scalar == ast::Type::Float
                    && !commonType(location, type, arg.type == ast::Type::Double ? scalar : arg.type, type))
                    return poison(ast::Type::Float);
            }
            for (const Value& arg : args)
                irArgs.push_back(convert(arg, type, location, Use::Passing));
            if (!builtin->runtime.empty() && !checkMathNames(location, *builtin))
                return poison(type);   // the program declares a function of a runtime function's name (Math.cpp)
            if (!builtin->runtime.empty() && math_ != Math::Fast)
                return {lowerMath(*builtin, type, irArgs[0]), type};   // the runtime's function (Math.cpp)
            llvm::Function* intrinsic =
                llvm::Intrinsic::getOrInsertDeclaration(&module_, builtin->intrinsic, {typeOf(type)});
            return {builder_.CreateCall(intrinsic, irArgs), type};
        }
        const auto found = functions_.find(call.callee);
        if (found == functions_.end())
        {
            std::vector<Diagnostic> notes;
            if (std::ranges::find(kLibmDoubles, call.callee) != kLibmDoubles.end())
                notes.push_back({location,
                                 "FMinus's " + call.callee + "f computes in float, and rounds a double argument to "
                                     "one; a prototype, 'double " + call.callee + "(double x);', calls the C "
                                     "library's on doubles",
                                 {}});
            diagnostics_.error(location, "call to undeclared function " + quoted(call.callee), std::move(notes));
            return poison(ast::Type::Float);
        }
        const ast::Function& callee = *found->second;
        calls_[function_->name].push_back({location, call.callee});
        if (args.size() != callee.params.size())
            return wrongArity(location, args.size(), callee.params.size());
        for (std::size_t i = 0; i < args.size(); ++i)
            irArgs.push_back(convert(args[i], callee.params[i].type, location, Use::Passing));
        return {builder_.CreateCall(module_.getFunction(call.callee), irArgs), callee.returnType};
    }

    // reduce_add(v), reduce_min(v), reduce_max(v): a float vector to one float. The sum is ordered, lane 0
    // first, as a loop would add them (llvm.vector.reduce.fadd without the reassoc flag), so it is exact and
    // reproducible; reassociating it into a tree is fast-math's to allow. min and max ignore NaN lanes.
    Lowering::Value Lowering::lowerReduction(SourceLocation location, const ast::CallExpr& call,
                                             const std::vector<Value>& args)
    {
        if (args.size() != 1)
            return wrongArity(location, args.size(), 1);
        const Value& vector = args[0];
        if (vector.failed && (!ast::isVector(vector.type) || ast::isMask(vector.type)))
            return recovery(ast::Type::Float);   // its own error says why (Value)
        if (!ast::isVector(vector.type) || ast::isMask(vector.type))
        {
            error(location, "the argument of " + quoted(call.callee) + " must be a float vector, not "
                                + quoted(ast::typeName(vector.type)));
            return poison(ast::Type::Float);
        }
        if (call.callee == "reduce_add")
            return {builder_.CreateFAddReduce(llvm::ConstantFP::getNegativeZero(builder_.getFloatTy()),
                                              vector.ir),
                    ast::Type::Float};
        if (call.callee == "reduce_min")
            return {builder_.CreateFPMinReduce(vector.ir), ast::Type::Float};
        return {builder_.CreateFPMaxReduce(vector.ir), ast::Type::Float};
    }

    // poly_load(p), poly_load(p, active), poly_store(p, v) and poly_store(p, v, active): OpenCL C's vloadn and vstoren
    // at the target's width, and Highway's masked forms. p is a const float* to load from (a float* or a void*
    // converts) and a float* to store to. The floats need only a float's alignment, as a buffer's do at any index, so
    // the load and store are unaligned vector ones; with a mask, llvm.masked.load (the lanes it leaves out 0) and
    // llvm.masked.store, which touch no memory in the lanes it leaves out.
    Lowering::Value Lowering::lowerPolyMemory(SourceLocation location, const ast::CallExpr& call,
                                              const std::vector<Value>& args)
    {
        const bool load = call.callee == "poly_load";
        const std::size_t arity = load ? 1 : 2;   // without the mask
        if (args.size() < arity)
            return wrongArity(location, args.size(), arity);
        if (args.size() > arity + 1)
            return wrongArity(location, args.size(), arity + 1);
        const llvm::Align align(4);
        llvm::Value* pointer =
            convert(args[0], ast::Type::pointerTo(ast::Kind::Float, load), location, Use::Passing);
        llvm::Value* value = load ? nullptr : convert(args[1], ast::Type::PolyFloat, location, Use::Passing);
        llvm::Value* mask =
            args.size() > arity ? convert(args[arity], ast::Type::PolyBool, location, Use::Passing) : nullptr;
        if (args[0].extent != nullptr && mask == nullptr)   // every lane, of an array the code generator sees
            checkBounds(location, *args[0].extent, elementOf(*args[0].extent, nullptr), polyLanes_);
        else if (args[0].extent != nullptr)   // each lane the mask takes (Bounds.cpp)
            checkLanes(location, *args[0].extent, mask);
        llvm::Type* vector = typeOf(ast::Type::PolyFloat);
        llvm::Instruction* access = nullptr;   // tagged as floats (Tbaa.cpp): a poly float is its lanes' type
        if (load && mask == nullptr)
            access = builder_.CreateAlignedLoad(vector, pointer, align, "poly.load");
        else if (load)
            access = builder_.CreateMaskedLoad(vector, pointer, align, mask, llvm::Constant::getNullValue(vector),
                                               "poly.load");
        else if (mask == nullptr)
            access = builder_.CreateAlignedStore(value, pointer, align);
        else
            access = builder_.CreateMaskedStore(value, pointer, align, mask);
        tag(access, ast::Type::PolyFloat);
        return load ? Value{access, ast::Type::PolyFloat} : Value{access, ast::Type::Void};
    }

    // malloc(size), calloc(count, size) and free(p): C's, with sizes in ints (C's are size_t), on the runtime's
    // functions, which take 64-bit sizes. malloc and calloc return a void* (null when the size is not positive or the
    // memory is not there), which converts to any pointer; free takes any pointer, or null.
    Lowering::Value Lowering::lowerAllocation(SourceLocation location, const Allocation& allocation,
                                              const std::vector<Value>& args)
    {
        if (args.size() != allocation.arity)
            return wrongArity(location, args.size(), allocation.arity);
        const ast::Type voidPointer = ast::Type::pointerTo(ast::Kind::Void, false);
        const std::string runtime(allocation.runtime);
        if (allocation.name == "free")
        {
            llvm::Value* pointer = convert(args[0], voidPointer, location, Use::Passing);
            return {callRuntime(runtime, builder_.getVoidTy(), {pointer}), ast::Type::Void};
        }
        std::vector<llvm::Value*> sizes;
        for (const Value& arg : args)
        {
            llvm::Value* size = convert(arg, ast::Type::Int, location, Use::Passing);
            sizes.push_back(builder_.CreateSExt(size, builder_.getInt64Ty(), "size"));
        }
        llvm::CallInst* allocate = callRuntime(runtime, builder_.getPtrTy(), sizes);
        allocate->setName(allocation.name);
        return {allocate, voidPointer};
    }

    // fft_create(n) and fft_create_complex(n): a plan, an fft_plan (null when the library takes no plan of that size),
    // to which a void* converts, as to any handle; fft_destroy(plan). fft_forward(plan, in, out) and fft_inverse(plan,
    // in, out) are overloaded on their buffers, as a math builtin is on its arguments: a forward transform of floats is
    // a real one, of float2s a complex one, and an inverse one is real when it gives floats. Each buffer converts as a
    // parameter of its type would, const added to the input's.
    Lowering::Value Lowering::lowerFft(SourceLocation location, const Fft& fft, const std::vector<Value>& args)
    {
        if (args.size() != fft.arity)
            return wrongArity(location, args.size(), fft.arity);
        const ast::Type plan = ast::Type::handleType(ast::Handle::FftPlan);
        if (fft.name == "fft_create" || fft.name == "fft_create_complex")
        {
            llvm::Value* n = convert(args[0], ast::Type::Int, location, Use::Passing);
            const std::string runtime =
                fft.name == "fft_create" ? "fminus_rt_fft_create" : "fminus_rt_fft_create_complex";
            return {callRuntime(runtime, builder_.getPtrTy(), {n}), plan};
        }
        llvm::Value* setup = convert(args[0], plan, location, Use::Passing);
        if (fft.name == "fft_destroy")
            return {callRuntime("fminus_rt_fft_destroy", builder_.getVoidTy(), {setup}), ast::Type::Void};

        const bool forward = fft.name == "fft_forward";
        const auto pointsTo = [](const Value& value, ast::Kind kind) {
            return ast::isPointer(value.type) && value.type.pointee == kind;
        };
        const Value& decides = forward ? args[1] : args[2];   // the floats' side
        const bool real = pointsTo(decides, ast::Kind::Float);
        if (!real && !pointsTo(decides, ast::Kind::Float2) && decides.failed)
            return recovery(ast::Type::Void);   // its own error says why (Value)
        if (!real && !pointsTo(decides, ast::Kind::Float2))
        {
            error(location, "no matching function for call to " + quoted(fft.name) + ": it takes "
                                + (forward ? "(fft_plan, const float *, float2 *) for a real plan, or (fft_plan, "
                                             "const float2 *, float2 *) for a complex one"
                                           : "(fft_plan, const float2 *, float *) for a real plan, or (fft_plan, "
                                             "const float2 *, float2 *) for a complex one"));
            return poison(ast::Type::Void);
        }
        const ast::Type in = ast::Type::pointerTo(forward && real ? ast::Kind::Float : ast::Kind::Float2, true);
        const ast::Type out = ast::Type::pointerTo(!forward && real ? ast::Kind::Float : ast::Kind::Float2, false);
        llvm::Value* input = convert(args[1], in, location, Use::Passing);
        llvm::Value* output = convert(args[2], out, location, Use::Passing);
        const std::string runtime = std::string("fminus_rt_fft_") + (forward ? "forward" : "inverse")
                                    + (real ? "_real" : "_complex");
        return {callRuntime(runtime, builder_.getVoidTy(), {setup, input, output}), ast::Type::Void};
    }

    // A builtin of the runtime's (kRuntimeBuiltins): each argument as its parameter says (Param), into a call of the
    // runtime's function, whose result is the builtin's (Result). A format's or a path's ints, after it, go to the
    // runtime in an array of the function's stack frame, with their count, and a reply's format's values in an array
    // of doubles; choices, the string literals after them, in one table. http_listen takes no argument, and passes the
    // program's web context and its route table (WebServer.cpp). One of a build option this build was configured
    // without is an error that names the option.
    Lowering::Value Lowering::lowerRuntimeBuiltin(SourceLocation location, const RuntimeBuiltin& builtin,
                                                  const ast::CallExpr& call)
    {
        std::vector<const ast::Expr*> args;
        for (const ast::ExprPtr& arg : call.args)
            args.push_back(arg.get());
        return lowerRuntimeCall(location, builtin, args, std::nullopt);
    }

    // The same, of `args`; with `first`, the builtin's first argument is that value, already lowered (a reply's
    // request: Serve.cpp), and args[0] is null in its place.
    Lowering::Value Lowering::lowerRuntimeCall(SourceLocation location, const RuntimeBuiltin& builtin,
                                               const std::vector<const ast::Expr*>& args, std::optional<Value> first)
    {
        const ast::Type type = resultType(builtin.result);
        if (!isBuilt(builtin.feature))
        {
            error(location, notBuilt(builtin.name, builtin.feature));
            return recovery(type);
        }
        if (builtin.effect != RuntimeEffect::WaitFree)
            calls_[function_->name].push_back({location, std::string(builtin.name)});   // what Effects.cpp checks
        const std::span<const Param> params(builtin.params.data(), builtin.arity);
        const auto format = std::ranges::find_if(params, [](Param param) {
            return param == Param::Format || param == Param::Path || param == Param::XmlPath || param == Param::Url
                   || param == Param::ReplyFormat || param == Param::FileName || param == Param::FilePattern;
        });
        // A URL's ints and a file's name's are the call's last arguments, after every other parameter's, as printf's
        // are; any other format's follow it.
        const bool trailing = format != params.end()
                              && (*format == Param::Url || *format == Param::FileName || *format == Param::FilePattern);
        const bool choices = !params.empty() && params.back() == Param::Choices;
        const std::size_t have = args.size();
        if (format == params.end() && !choices && have != params.size())
        {
            wrongArity(location, have, params.size());
            return recovery(type);
        }
        if (have < params.size())
        {
            error(location, "too few arguments to function call, expected at least " + std::to_string(params.size())
                                + ", have " + std::to_string(have));
            return recovery(type);
        }
        // A format's ints follow it (a URL's and a file's name's end the call): every argument the other parameters
        // leave, or, before choices, every one up to the first string literal.
        const std::size_t formatAt = static_cast<std::size_t>(format - params.begin());
        const std::size_t intsAt = trailing ? params.size() : formatAt + 1;
        std::size_t ints = have - params.size();
        if (choices)
        {
            ints = 0;
            while (formatAt + 1 + ints < have && !isString(*args[formatAt + 1 + ints]))
                ++ints;
            if (formatAt + 1 + ints == have)
            {
                error(location, "too few arguments to function call, expected at least "
                                    + std::to_string(params.size() + ints) + ", have " + std::to_string(have));
                return recovery(type);
            }
        }
        std::vector<llvm::Value*> irArgs;
        bool fine = true;
        for (std::size_t i = 0; i < params.size(); ++i)
        {
            const std::size_t at = i > formatAt && !trailing ? i + ints : i;
            if (args[at] == nullptr)   // the value given for it
            {
                irArgs.push_back(convert(*first, *paramType(params[i]), location, Use::Passing));
                continue;
            }
            const ast::Expr& arg = *args[at];
            const Param param = params[i];
            if (param == Param::Choices)
            {
                std::vector<const ast::StringExpr*> literals;
                for (std::size_t k = at; k < have; ++k)
                {
                    if (!isString(*args[k]))
                    {
                        error(begin(*args[k]), "argument to " + quoted(builtin.name) + " must be a string literal");
                        fine = false;
                        continue;
                    }
                    literals.push_back(&std::get<ast::StringExpr>(args[k]->node));
                }
                irArgs.push_back(preparedChoices(literals));
                continue;
            }
            if (param == Param::String || param == Param::Format || param == Param::Path || param == Param::XmlPath
                || param == Param::Url || param == Param::ContentType || param == Param::Route
                || param == Param::QueryName || param == Param::ReplyFormat || param == Param::FileName
                || param == Param::FilePattern)
            {
                if (!isString(arg))
                {
                    error(begin(arg), "argument to " + quoted(builtin.name) + " must be a string literal");
                    fine = false;
                    continue;
                }
                const auto& literal = std::get<ast::StringExpr>(arg.node);
                if ((param == Param::Url && !checkUrl(literal, begin(arg)))
                    || (param == Param::ContentType && !checkContentType(literal, begin(arg)))
                    || (param == Param::Route && !checkRoute(literal, begin(arg)))
                    || (param == Param::QueryName && !checkQueryName(literal, begin(arg)))
                    || ((param == Param::FileName || param == Param::FilePattern)
                        && !checkFileName(literal, begin(arg), builtin.name)))
                {
                    fine = false;
                    continue;
                }
                if (param == Param::Url)
                    irArgs.push_back(webContext(location));   // the program's web context, then the URL (Web.cpp)
                if (param == Param::FileName || param == Param::FilePattern)
                    irArgs.push_back(filesContext());   // the program's file context, then the name (Files.cpp)
                if (param == Param::Path || param == Param::XmlPath)
                {
                    llvm::Value* path =
                        param == Param::Path ? preparedPath(literal) : preparedXmlPath(literal, begin(arg));
                    fine = fine && path != nullptr;
                    irArgs.push_back(path);
                }
                else
                {
                    irArgs.push_back(stringConstant(literal));
                    irArgs.push_back(builder_.getInt32(static_cast<std::uint32_t>(literal.value.size())));
                }
                if (param == Param::Route)   // what Program::webRoutes() lists, and the route table
                    recordWebRoute(location, literal, begin(arg));
                if (param == Param::String || param == Param::ContentType || param == Param::Route
                    || param == Param::QueryName)
                    continue;
                std::vector<const ast::Expr*> given;
                for (std::size_t k = 0; k < ints; ++k)
                    given.push_back(args[intsAt + k]);
                if (param == Param::ReplyFormat)   // its values, doubles (WebServer.cpp)
                {
                    const std::optional<std::vector<llvm::Value*>> values = replyArguments(literal, given);
                    if (!values)
                    {
                        fine = false;
                        continue;
                    }
                    irArgs.push_back(replyValues(*values));
                    irArgs.push_back(builder_.getInt32(static_cast<std::uint32_t>(values->size())));
                    continue;
                }
                const std::optional<std::vector<llvm::Value*>> values = formatArguments(literal, given);
                if (!values)
                {
                    fine = false;
                    continue;
                }
                if (param == Param::Url)   // what Program::webRequests() lists
                    recordWebRequest(location, builtin.name == "http_post" ? "POST" : "GET", literal);
                if (param == Param::FileName || param == Param::FilePattern)   // what Program::files() lists
                    recordFileUse(location, builtin.name.substr(5), literal);   // "read", "list", "write", "append"
                irArgs.push_back(formatInts(*values));
                irArgs.push_back(builder_.getInt32(static_cast<std::uint32_t>(values->size())));
                continue;
            }
            ast::Type to = *paramType(param);
            const std::optional<ast::Type> other = otherType(param);
            const Value value = isString(arg) ? refuseString(arg, to, Use::Passing, arg.location) : lowerAs(arg, to);
            fine = fine && !value.failed;
            if (other && value.type == *other)
                to = *other;   // the second of the two handles it takes
            else if (other && !value.failed && ast::isHandle(value.type) && value.type != to)
            {
                // A handle of a third kind: C's warning for it, with both the types it takes.
                error(location, "incompatible pointer types passing " + quoted(ast::typeName(value.type))
                                    + " to parameter of type " + quoted(ast::typeName(to)) + " or "
                                    + quoted(ast::typeName(*other)));
                fine = false;
                continue;
            }
            // A read's document may be a const void*, as it was before FMinus had handles: the read only reads it.
            const bool readOnly = (param == Param::JsonRead || param == Param::XmlRead) && ast::isPointer(value.type)
                                  && value.type.pointee == ast::Kind::Void;
            llvm::Value* converted = readOnly ? value.ir : convert(value, to, location, Use::Passing);
            if (param == Param::Bool)   // to the runtime as an int, 0 or 1
                converted = builder_.CreateZExt(converted, builder_.getInt32Ty());
            if (param == Param::Text && !builtin.symbol.empty())   // to the runtime as its pointer and its length
            {
                irArgs.push_back(builder_.CreateExtractValue(converted, 0, "data"));
                irArgs.push_back(builder_.CreateExtractValue(converted, 1, "size"));
                continue;
            }
            irArgs.push_back(converted);
        }
        if (!fine)
            return recovery(type);
        if (builtin.symbol.empty())   // made inline: a string's (Text.cpp)
            return lowerText(location, builtin, irArgs);
        const bool listen = builtin.symbol == "fminus_web_http_listen";
        if (listen)
            irArgs = listenArguments(location);
        llvm::Type* returned = builtin.result == Result::Bool ? builder_.getInt32Ty() : typeOf(type);
        llvm::CallInst* result = callRuntime(std::string(builtin.symbol), returned, irArgs);
        if (listen)
            webListens_.push_back(result);   // its route table, once every route is known (finishWebRoutes)
        if (builtin.result == Result::Void)
            return {result, type};
        result->setName(builtin.name);
        if (builtin.result == Result::Bool)
            return {builder_.CreateICmpNE(result, builder_.getInt32(0), "tobool"), type};
        return {result, type};
    }

    // A format's ints, in an array of the function's stack frame: its address, or null for none.
    llvm::Value* Lowering::formatInts(const std::vector<llvm::Value*>& values)
    {
        if (values.empty())
            return llvm::ConstantPointerNull::get(builder_.getPtrTy());
        llvm::AllocaInst* array = allocate(llvm::ArrayType::get(builder_.getInt32Ty(), values.size()), "format.ints");
        for (std::size_t k = 0; k < values.size(); ++k)
            tag(builder_.CreateStore(values[k], builder_.CreateConstInBoundsGEP2_32(array->getAllocatedType(), array, 0,
                                                                                   static_cast<unsigned>(k))),
                ast::Type::Int);
        return array;
    }

    // A call of the runtime's function `name` (runtime/Runtime.h), declared from the arguments' IR types, with the
    // attributes that say what it does (describeRuntime). Not when the program declares a function of that name itself:
    // its prototype may give other types, and its definition would be its own code, not the runtime's.
    llvm::CallInst* Lowering::callRuntime(const std::string& name, llvm::Type* result,
                                          const std::vector<llvm::Value*>& args)
    {
        std::vector<llvm::Type*> types;
        for (llvm::Value* arg : args)
            types.push_back(arg->getType());
        llvm::FunctionCallee function =
            module_.getOrInsertFunction(name, llvm::FunctionType::get(result, types, false));
        if (auto* declared = llvm::dyn_cast<llvm::Function>(function.getCallee());
            declared != nullptr && !functions_.contains(name))
            describeRuntime(*declared);
        return builder_.CreateCall(function, args);
    }

    Lowering::Value Lowering::wrongArity(SourceLocation location, std::size_t have, std::size_t expected)
    {
        error(location, std::string(have > expected ? "too many" : "too few") + " arguments to function call, expected "
                            + std::to_string(expected) + ", have " + std::to_string(have));
        return poison(ast::Type::Float);
    }
} // namespace fminus::ir::detail

namespace fminus::ir
{
    std::vector<BuiltinInfo> builtins()
    {
        std::vector<BuiltinInfo> list;
        for (const detail::Builtin& builtin : detail::kBuiltins)
            list.push_back({builtin.name, builtin.arity});
        return list;
    }
} // namespace fminus::ir
