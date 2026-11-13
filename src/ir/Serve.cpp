// src/ir/Serve.cpp: serve, route and reply, FMinus's statements over the web server's builtins (docs/web.md, "The
// server"; docs/research/syntax/README.md, §5), and the JSON literal a reply writes (§6.1). In a build with
// FMINUS_WEB_SERVER; in one without it, a serve is the error the server's builtins give there (notBuilt).
//
// `serve (server) { route ... }` is the loop and the chain a program wrote before, with one function of the library's
// more: it takes the requests waiting for its own routes alone (fminus_web_http_next_in, of a route set, a constant of
// the program's: one bit for each of its routes' indexes in the route table), and switches on the route each was
// published for (fminus_web_http_route, which also makes that route's places the request's, for http_arg):
//
//       %serve.take = call ptr @fminus_web_http_next_in(ptr %server, ptr @serve.routes, i32 1)
//       store ptr %serve.take, ptr %serve.request
//       br label %serve.cond
//     serve.cond:                   ; a request waiting for one of its routes, or the serve is done
//       %request = load ptr, ptr %serve.request
//       %serve.none = icmp eq ptr %request, null
//       br i1 %serve.none, label %serve.end, label %serve.body
//     serve.body:
//       %route = call i32 @fminus_web_http_route(ptr %request)
//       switch i32 %route, label %serve.default [ i32 0, label %serve.route ... ]
//     serve.route:                  ; its parameters filled and checked (400 on serve.bad), then its block
//       ...
//     serve.next:                   ; where each reply goes on to
//       %serve.take1 = call ptr @fminus_web_http_next_in(...)
//       ...
//
// With a timeout, `serve (server, ms)`, its first take is fminus_web_http_accept_in, which waits: so it blocks, and a
// nonblocking function refuses it as it refuses http_accept (Effects.cpp). A route's block is a block of the function
// (its scope's names, its effects), its parameters const variables of it; a reply answers (http_reply, _bytes or
// _text, each checked as the builtin checks its arguments), answers 500 when the library refused the reply and still
// holds the request (HTTP_LIMIT, HTTP_INVALID), runs the deferred statements of the route's blocks, and goes to
// serve.next. A route whose block can reach its end without a reply is an error at its closing brace, the missing
// return's analysis on the route's own blocks.
//
// A JSON literal, a reply's body, is a format of the reply's that the compiler makes: its keys and its structure
// constant text, each value a place, %r (a float's shortest digits), %R (a double's), %d (an int) or %b (a bool's
// words), which the library writes into the request's buffer (web/Server.cpp): no JSON value is ever made.
#include "ir/Lowering.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <set>

namespace fminus::ir::detail
{
    namespace
    {
        // The library's answers a reply's refusal is told by (web/Web.h): the request is still the caller's.
        constexpr std::int32_t kWebLimit = -5;     // FMINUS_WEB_LIMIT
        constexpr std::int32_t kWebInvalid = -7;   // FMINUS_WEB_INVALID

        constexpr std::size_t kMaxQueryName = 64;   // the query's names, as http_query_float's (WebServer.cpp)

        constexpr const char* kReplyForms = "a reply is 'reply status;', 'reply status, data, bytes, \"type\";', "
                                            "'reply status, \"type\", \"format\", values...;' or 'reply status, "
                                            "json { ... };'";

        // The last of `attributes` named `name`: the one that counts, as a field's (Program::findAttribute).
        const ast::Attribute* lastNamed(const std::vector<ast::Attribute>& attributes, std::string_view name)
        {
            const ast::Attribute* found = nullptr;
            for (const ast::Attribute& attribute : attributes)
            {
                if (attribute.name == name)
                    found = &attribute;
            }
            return found;
        }

        // A parameter's fminus::range, when it has a good one (checkAttributes reports a bad one).
        std::optional<std::pair<double, double>> rangeOf(const ast::RouteParam& param)
        {
            const ast::Attribute* range = lastNamed(param.attributes, "fminus::range");
            if (range == nullptr || range->args.size() != 2)
                return std::nullopt;
            const std::optional<double> low = ast::attributeNumber(range->args[0]);
            const std::optional<double> high = ast::attributeNumber(range->args[1]);
            if (!low || !high || *low >= *high)
                return std::nullopt;
            return std::pair(*low, *high);
        }

        std::optional<double> defaultOf(const ast::RouteParam& param)
        {
            const ast::Attribute* fallback = lastNamed(param.attributes, "fminus::default");
            if (fallback == nullptr || fallback->args.size() != 1)
                return std::nullopt;
            return ast::attributeNumber(fallback->args[0]);
        }

        // Where the `offset`-th byte of a literal is spelt, or the literal's own place.
        SourceLocation byteAt(const ast::StringExpr& literal, std::size_t offset, SourceLocation where)
        {
            return offset < literal.at.size() ? literal.at[offset] : where;
        }

        // A key of a JSON literal, written into `out` as a JSON string's contents (RFC 8259, §7: '"', '\\' and the
        // control characters escaped), each '%' doubled for the format it is part of. The offset of a byte that is no
        // part of UTF-8 text, which no JSON string may hold, or none.
        std::optional<std::size_t> jsonKey(std::string_view key, std::string& out)
        {
            for (std::size_t i = 0; i < key.size();)
            {
                const auto byte = static_cast<unsigned char>(key[i]);
                if (byte >= 0x80)
                {
                    // UTF-8: a lead byte and its continuation bytes, the shortest form, no surrogate, at most U+10FFFF.
                    const std::size_t length = byte >= 0xc2 && byte <= 0xdf   ? 2
                                               : byte >= 0xe0 && byte <= 0xef ? 3
                                               : byte >= 0xf0 && byte <= 0xf4 ? 4
                                                                               : 0;
                    if (length == 0 || i + length > key.size())
                        return i;
                    for (std::size_t k = 1; k < length; ++k)
                    {
                        if ((static_cast<unsigned char>(key[i + k]) & 0xc0) != 0x80)
                            return i;
                    }
                    const auto second = static_cast<unsigned char>(key[i + 1]);
                    if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0)
                        || (byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90))
                        return i;
                    out.append(key.substr(i, length));
                    i += length;
                    continue;
                }
                switch (key[i])
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                case '%':
                    out += "%%";
                    break;
                default:
                    if (byte < 0x20)
                    {
                        constexpr std::string_view kHex = "0123456789abcdef";
                        out += "\\u00";
                        out += kHex[byte >> 4];
                        out += kHex[byte & 15];
                    }
                    else
                        out += key[i];
                    break;
                }
                ++i;
            }
            return std::nullopt;
        }
    } // namespace

    // The innermost of the loops, switches, regions, deferred statements and routes around that is one of `kinds`.
    const Lowering::Breakable* Lowering::innermost(std::initializer_list<Breakable::Kind> kinds) const
    {
        for (auto around = breakables_.rbegin(); around != breakables_.rend(); ++around)
        {
            if (std::ranges::find(kinds, around->kind) != kinds.end())
                return &*around;
        }
        return nullptr;
    }

    // A route's parameters and holes, checked: each parameter an int, a float, a bool or an http_request (one at
    // most), its name its own; a hole's an int, without a default; another's name one a query may have; their
    // attributes a module parameter's, checked as there; each hole a different parameter's. No attribute after the
    // list. Their description for the host (Program::webRoutes), into `parameters`. Whether all is well.
    bool Lowering::checkRouteParameters(const ast::Route& route, const RouteHoles& holes, llvm::Metadata*& parameters)
    {
        const std::size_t reported = diagnostics_.errors().size();
        if (!route.attributes.empty())
            error(route.attributes.front().location,
                  quoted(route.attributes.front().name) + " attribute cannot be applied to a route: a route is a block "
                                                          "of the function its serve is in, whose attributes cover it");
        llvm::LLVMContext& context = module_.getContext();
        const auto number = [&](std::optional<double> value) {
            std::vector<llvm::Metadata*> operands;
            if (value)
                operands.push_back(llvm::ConstantAsMetadata::get(llvm::ConstantFP::get(builder_.getDoubleTy(), *value)));
            return llvm::MDTuple::get(context, operands);
        };
        std::vector<llvm::Metadata*> described;
        std::set<std::string> names;
        const ast::RouteParam* request = nullptr;
        for (const ast::RouteParam& param : route.params)
        {
            const ast::Type type = param.type;
            const bool isRequest = type == ast::Type::handleType(ast::Handle::HttpRequest);
            if (type != ast::Type::Int && type != ast::Type::Float && type != ast::Type::Bool && !isRequest)
            {
                error(param.location, "a route's parameter is an int, a float, a bool or an http_request, not "
                                          + quoted(ast::typeName(type)));
                continue;
            }
            if (!names.insert(param.name).second)
            {
                error(param.location, "redefinition of parameter " + quoted(param.name));
                continue;
            }
            const auto hole = std::ranges::find(holes.holes, param.name, &RouteHoles::Hole::name);
            const bool inPath = hole != holes.holes.end();
            if (isRequest)
            {
                if (request != nullptr)
                    diagnostics_.error(param.location, "a route has one http_request parameter at most, its request",
                                       {Diagnostic{request->location, "the route's request is here", {}}});
                else if (!param.attributes.empty())
                    error(param.attributes.front().location,
                          quoted(param.attributes.front().name) + " attribute does not apply to the route's request");
                else if (inPath)
                    error(param.location, "parameter " + quoted(param.name) + " fills the hole {" + param.name
                                              + "}, a number of the path: it is an int, not 'http_request'");
                request = &param;
                described.push_back(llvm::MDTuple::get(
                    context, {llvm::MDString::get(context, param.name), llvm::MDString::get(context, "http_request"),
                              llvm::MDString::get(context, "request"), number(std::nullopt), number(std::nullopt),
                              number(std::nullopt)}));
                continue;
            }
            if (inPath && type != ast::Type::Int)
                error(param.location, "parameter " + quoted(param.name) + " fills the hole {" + param.name
                                          + "}, a number of the path: it is an int, not " + quoted(ast::typeName(type)));
            if (!inPath && param.name.size() > kMaxQueryName)
                error(param.location, "parameter " + quoted(param.name) + " is read from the query's field of its "
                                          "name, which is 1 to 64 bytes, not " + std::to_string(param.name.size()));
            // A module parameter's attributes, checked as its are (Modules.cpp).
            const ast::Field field{param.location, param.name, type, false, false, true, param.attributes, nullptr, 0,
                                   nullptr};
            checkAttributes(field, type, false);
            for (const ast::Attribute& attribute : param.attributes)
            {
                if (attribute.name == "fminus::default" && inPath)
                    error(attribute.location, "'fminus::default' does not apply to a hole's parameter: a request "
                                              "the route takes always has its number");
                else if (attribute.name != "fminus::range" && attribute.name != "fminus::default"
                         && !ast::isIgnoredAttribute(attribute.name) && !attribute.name.starts_with("fminus::")
                         && !attribute.name.starts_with("clang::"))
                    error(attribute.location, "unknown attribute " + quoted(attribute.name)
                                                  + ": a route's parameter takes fminus::range(min, max) and "
                                                    "fminus::default(value)");
            }
            const std::optional<double> fallback = defaultOf(param);
            if (fallback && !inPath && type == ast::Type::Int
                && (std::trunc(*fallback) != *fallback || *fallback < -2147483648.0 || *fallback > 2147483647.0))
                error(lastNamed(param.attributes, "fminus::default")->location,
                      "the default of an int parameter is an int, not " + std::format("{}", *fallback));
            else if (fallback && !inPath && type == ast::Type::Bool && *fallback != 0 && *fallback != 1)
                error(lastNamed(param.attributes, "fminus::default")->location,
                      "the default of a bool parameter is 0 or 1, not " + std::format("{}", *fallback));
            const std::optional<std::pair<double, double>> range = rangeOf(param);
            described.push_back(llvm::MDTuple::get(
                context, {llvm::MDString::get(context, param.name), llvm::MDString::get(context, ast::typeName(type)),
                          llvm::MDString::get(context, inPath ? "path" : "query"),
                          number(range ? std::optional(range->first) : std::nullopt),
                          number(range ? std::optional(range->second) : std::nullopt),
                          number(inPath ? std::nullopt : fallback)}));
        }
        std::set<std::string> filled;
        for (const RouteHoles::Hole& hole : holes.holes)
        {
            const SourceLocation where = byteAt(route.literal, hole.offset, route.literalLocation);
            if (std::ranges::find(route.params, hole.name, &ast::RouteParam::name) == route.params.end())
                error(where, "hole {" + hole.name + "} names no parameter of the route");
            else if (!filled.insert(hole.name).second)
                error(where, "parameter " + quoted(hole.name) + " fills a hole of the route already: each hole is "
                                                                 "another parameter's");
        }
        parameters = llvm::MDTuple::get(context, described);
        return diagnostics_.errors().size() == reported;
    }

    // `serve (server) { routes }`, or `serve (server, timeout_ms) { routes }`: the server and the timeout computed
    // once; the first request taken (by accept_in with a timeout, which waits; by next_in without), then each route's
    // block for each request, and the next taken, until none is waiting. Each route's literal checked and in the route
    // table, the serve's set of them a constant of the program.
    void Lowering::lowerServe(SourceLocation location, const ast::ServeStmt& serve)
    {
        if (!isBuilt(Feature::WebServer))
        {
            error(location, notBuilt("serve", Feature::WebServer));
            return;
        }
        if (innermost({Breakable::Kind::Route}) != nullptr)
        {
            error(location, "a route cannot hold a serve: a serve takes its own routes' requests, and a route ends "
                            "with a reply to its own");
            return;
        }
        if (serve.args.size() > 2)
        {
            error(begin(*serve.args[2]), "a serve takes a server and, to wait for a request, a timeout in "
                                         "milliseconds: serve (server) or serve (server, timeout_ms)");
            return;
        }
        if (serve.routes.empty())
            error(location, "a serve has no route: it takes the requests waiting for its routes, and has none");
        const auto argument = [&](const ast::Expr& arg, ast::Type type) {
            const Value value = isString(arg) ? refuseString(arg, type, Use::Passing, begin(arg)) : lowerAs(arg, type);
            return convert(value, type, begin(arg), Use::Passing);
        };
        const ast::Type serverType = ast::Type::handleType(ast::Handle::HttpServer);
        const ast::Type requestType = ast::Type::handleType(ast::Handle::HttpRequest);
        llvm::Value* server = argument(*serve.args[0], serverType);
        llvm::Value* timeout = serve.args.size() == 2 ? argument(*serve.args[1], ast::Type::Int) : nullptr;
        if (timeout != nullptr)
            calls_[function_->name].push_back({location, "http_accept"});   // it waits as http_accept does

        // The routes: each literal checked, its parameters too, and its index in the route table.
        std::vector<RouteHoles> holes;
        std::vector<std::optional<std::size_t>> indexes;
        std::vector<std::uint64_t> set;
        for (const ast::Route& route : serve.routes)
        {
            holes.push_back(scanRouteHoles(route.literal.value));
            const RouteHoles& scan = holes.back();
            bool fine = scan.errors.empty();
            if (!fine)
                error(byteAt(route.literal, scan.errors.front().offset, route.literalLocation),
                      scan.errors.front().message);
            llvm::Metadata* parameters = nullptr;
            fine = checkRouteParameters(route, scan, parameters) && fine;
            indexes.emplace_back();
            if (!fine)
                continue;
            const std::size_t index = routeIndex(scan.library);
            recordRouteStatement(route, scan.library, parameters);
            indexes.back() = index;
            if (set.size() <= index / 64)
                set.resize(index / 64 + 1);
            set[index / 64] |= std::uint64_t{1} << (index % 64);
        }
        llvm::Value* routes = llvm::ConstantPointerNull::get(builder_.getPtrTy());
        if (!set.empty())
        {
            auto* type = llvm::ArrayType::get(builder_.getInt64Ty(), set.size());
            auto* global = new llvm::GlobalVariable(module_, type, true, llvm::GlobalValue::PrivateLinkage,
                                                    llvm::ConstantDataArray::get(module_.getContext(), set),
                                                    "serve.routes");
            global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
            routes = global;
        }
        llvm::Value* words = builder_.getInt32(static_cast<std::uint32_t>(set.size()));
        llvm::Type* pointer = builder_.getPtrTy();
        const auto take = [&](bool first) {
            llvm::CallInst* taken =
                first && timeout != nullptr
                    ? callRuntime("fminus_web_http_accept_in", pointer, {server, routes, words, timeout})
                    : callRuntime("fminus_web_http_next_in", pointer, {server, routes, words});
            taken->setName("serve.take");
            return taken;
        };

        llvm::AllocaInst* slot = allocate(pointer, "serve.request");
        tag(builder_.CreateStore(take(true), slot), requestType);
        llvm::BasicBlock* condBlock = newBlock("serve.cond");
        llvm::BasicBlock* bodyBlock = newBlock("serve.body");
        llvm::BasicBlock* otherBlock = newBlock("serve.default");
        llvm::BasicBlock* badBlock = newBlock("serve.bad");
        llvm::BasicBlock* nextBlock = newBlock("serve.next");
        llvm::BasicBlock* endBlock = newBlock("serve.end");
        const llvm::Instruction* entry = builder_.CreateBr(condBlock);

        builder_.SetInsertPoint(condBlock);
        llvm::LoadInst* waiting = builder_.CreateLoad(pointer, slot, "request");
        tag(waiting, requestType);
        builder_.CreateCondBr(builder_.CreateIsNull(waiting, "serve.none"), endBlock, bodyBlock);

        builder_.SetInsertPoint(bodyBlock);
        llvm::LoadInst* request = builder_.CreateLoad(pointer, slot, "request");
        tag(request, requestType);
        llvm::Value* which = callRuntime("fminus_web_http_route", builder_.getInt32Ty(), {request});
        which->setName("route");
        llvm::SwitchInst* dispatch = builder_.CreateSwitch(which, otherBlock);
        std::set<std::size_t> cased;
        for (std::size_t k = 0; k < serve.routes.size(); ++k)
        {
            llvm::BasicBlock* routeBlock = newBlock("serve.route");
            if (indexes[k] && cased.insert(*indexes[k]).second)
                dispatch->addCase(builder_.getInt32(static_cast<std::uint32_t>(*indexes[k])), routeBlock);
            builder_.SetInsertPoint(routeBlock);
            lowerRoute(serve.routes[k], slot, nextBlock, badBlock, holes[k]);
        }

        // A request the library published for none of the serve's routes, which it never does: answered, so that it
        // is consumed all the same. And a request whose parameters do not fit: 400, before its route's block.
        const auto answer = [&](llvm::BasicBlock* block, std::int32_t status) {
            block->moveAfter(&block->getParent()->back());
            builder_.SetInsertPoint(block);
            llvm::LoadInst* answered = builder_.CreateLoad(pointer, slot, "request");
            tag(answered, requestType);
            callRuntime("fminus_web_http_reply", builder_.getInt32Ty(),
                        {answered, builder_.getInt32(static_cast<std::uint32_t>(status))});
            builder_.CreateBr(nextBlock);
        };
        answer(otherBlock, 500);
        answer(badBlock, 400);
        nextBlock->moveAfter(&nextBlock->getParent()->back());
        builder_.SetInsertPoint(nextBlock);
        tag(builder_.CreateStore(take(false), slot), requestType);
        builder_.CreateBr(condBlock);
        mustProgress(condBlock, entry);
        endBlock->moveAfter(&endBlock->getParent()->back());
        builder_.SetInsertPoint(endBlock);
    }

    // A route's block, at the switch's case for it: its parameters, const variables of its outermost scope (as a
    // function's are of its body's), filled from the request and checked, any that does not fit the 400 of `bad`; then
    // its block, which every path through must leave by a reply. A route whose literal or parameters were in error is
    // lowered all the same, for its block's errors, where nothing reaches it.
    void Lowering::lowerRoute(const ast::Route& route, llvm::Value* request, llvm::BasicBlock* next,
                              llvm::BasicBlock* bad, const RouteHoles& holes)
    {
        llvm::BasicBlock* start = builder_.GetInsertBlock();
        scopes_.emplace_back();
        breakables_.push_back({Breakable::Kind::Route, nullptr, nullptr, deferScopes_.size(), route.location});
        routes_.push_back({request, next, deferScopes_.size(), 0});
        llvm::LoadInst* taken = builder_.CreateLoad(builder_.getPtrTy(), request, "request");
        tag(taken, ast::Type::handleType(ast::Handle::HttpRequest));
        for (const ast::RouteParam& param : route.params)
        {
            const auto hole = std::ranges::find(holes.holes, param.name, &RouteHoles::Hole::name);
            fillParameter(param,
                          hole != holes.holes.end()
                              ? std::optional(static_cast<std::size_t>(hole - holes.holes.begin()))
                              : std::nullopt,
                          taken, bad);
        }
        openDefers();
        lowerStatements(route.body);
        closeDefers();
        if (!builder_.GetInsertBlock()->hasTerminator())
        {
            // Whether the route's start reaches its end, through its own blocks (the serve's are not finished yet,
            // and a reply's way out, to `next` or `bad`, leaves the route): a constant condition's branch is one way
            // (branchOn), as for a function's end (Functions.cpp).
            llvm::BasicBlock* end = builder_.GetInsertBlock();
            builder_.CreateBr(next);
            std::vector<llvm::BasicBlock*> work{start};
            std::set<llvm::BasicBlock*> seen{start};
            bool reaches = false;
            while (!work.empty() && !reaches)
            {
                llvm::BasicBlock* block = work.back();
                work.pop_back();
                reaches = block == end;
                for (llvm::BasicBlock* successor : llvm::successors(block))
                {
                    if (successor != next && successor != bad && seen.insert(successor).second)
                        work.push_back(successor);
                }
            }
            if (reaches)
                error(route.closing, "route " + quoted(route.literal.value) + " does not reply"
                                         + (routes_.back().replies > 0 ? " in all control paths" : ""));
        }
        routes_.pop_back();
        breakables_.pop_back();
        scopes_.pop_back();
    }

    // A route's parameter, filled from the request: a hole's (`hole`, its place) from the path, as http_arg reads it;
    // another's from the query's field of its name, missing its default or a 400, not a value of its type a 400 (an
    // int as http_query_int reads one, called with two fallbacks that a value cannot both be; a float as
    // http_query_float, with a NaN for its fallback, which no value is; a bool true, false, 1 or 0, its bytes
    // compared); then its range, outside it a 400; the request itself for an http_request. A const variable.
    void Lowering::fillParameter(const ast::RouteParam& param, std::optional<std::size_t> hole, llvm::Value* request,
                                 llvm::BasicBlock* bad)
    {
        const ast::Type type = param.type;
        const bool isRequest = type == ast::Type::handleType(ast::Handle::HttpRequest);
        if (type != ast::Type::Int && type != ast::Type::Float && type != ast::Type::Bool && !isRequest)
        {
            // checkRouteParameters reported it: a variable of its type all the same, 0, so that its uses report
            // nothing more.
            if (type == ast::Type::Void)
                return;
            llvm::AllocaInst* slot = allocate(typeOf(type), param.name);
            if (ast::isModule(type))
                zero(slot, type);
            else
                builder_.CreateStore(llvm::Constant::getNullValue(typeOf(type)), slot);
            bind(param.name, Binding{slot, true, type, param.location});
            return;
        }
        llvm::AllocaInst* slot = allocate(typeOf(type), param.name);
        const auto declare = [&] { bind(param.name, Binding{slot, true, type, param.location}); };
        if (isRequest)
        {
            tag(builder_.CreateStore(request, slot), type);
            declare();
            return;
        }
        llvm::Type* i32 = builder_.getInt32Ty();
        if (hole)
            tag(builder_.CreateStore(callRuntime("fminus_web_http_arg", i32,
                                                 {request, builder_.getInt32(static_cast<std::uint32_t>(*hole))}),
                                     slot),
                type);
        else
        {
            ast::StringExpr name{param.name, {}};
            llvm::Value* text = stringConstant(name);
            llvm::Value* length = builder_.getInt32(static_cast<std::uint32_t>(param.name.size()));
            llvm::AllocaInst* bytes = type == ast::Type::Bool ? allocate(i32, param.name + ".length") : nullptr;
            llvm::Value* found = callRuntime("fminus_web_http_query_text", builder_.getPtrTy(),
                                             {request, text, length,
                                              bytes != nullptr ? static_cast<llvm::Value*>(bytes)
                                                               : llvm::ConstantPointerNull::get(builder_.getPtrTy())});
            found->setName(param.name + ".text");
            llvm::BasicBlock* missing = newBlock("param.missing");
            llvm::BasicBlock* read = newBlock("param.read");
            llvm::BasicBlock* done = newBlock("param.done");
            builder_.CreateCondBr(builder_.CreateIsNull(found, param.name + ".missing"), missing, read);

            builder_.SetInsertPoint(missing);
            if (const std::optional<double> fallback = defaultOf(param))
            {
                // A float's rounded to the nearest float, as C converts a constant (ConstantFP::get).
                llvm::Value* value =
                    type == ast::Type::Float
                        ? static_cast<llvm::Value*>(llvm::ConstantFP::get(builder_.getFloatTy(), *fallback))
                    : type == ast::Type::Int
                        ? builder_.getInt32(static_cast<std::uint32_t>(static_cast<std::int32_t>(*fallback)))
                        : builder_.getInt1(*fallback != 0);
                tag(builder_.CreateStore(value, slot), type);
                builder_.CreateBr(done);
            }
            else
                builder_.CreateBr(bad);

            builder_.SetInsertPoint(read);
            if (type == ast::Type::Int)
            {
                llvm::Value* zero =
                    callRuntime("fminus_web_http_query_int", i32, {request, text, length, builder_.getInt32(0)});
                llvm::Value* one =
                    callRuntime("fminus_web_http_query_int", i32, {request, text, length, builder_.getInt32(1)});
                tag(builder_.CreateStore(zero, slot), type);
                builder_.CreateCondBr(builder_.CreateICmpEQ(zero, one, param.name + ".number"), done, bad);
            }
            else if (type == ast::Type::Float)
            {
                llvm::Value* nan = llvm::ConstantFP::getNaN(builder_.getFloatTy());
                llvm::Value* value =
                    callRuntime("fminus_web_http_query_float", builder_.getFloatTy(), {request, text, length, nan});
                tag(builder_.CreateStore(value, slot), type);
                builder_.CreateCondBr(builder_.CreateFCmpORD(value, value, param.name + ".number"), done, bad);
            }
            else
            {
                // true, false, 1 or 0: its length, then its bytes.
                llvm::LoadInst* count = builder_.CreateLoad(i32, bytes, param.name + ".length");
                tag(count, ast::Type::Int);
                llvm::BasicBlock* isTrue = newBlock("param.true");
                llvm::BasicBlock* isFalse = newBlock("param.false");
                constexpr std::array<std::pair<std::string_view, bool>, 4> kWords{
                    {{"true", true}, {"false", false}, {"1", true}, {"0", false}}};
                for (const auto& [word, value] : kWords)
                {
                    llvm::BasicBlock* bytesBlock = newBlock("param.word");
                    llvm::BasicBlock* otherwise = newBlock("param.next");
                    builder_.CreateCondBr(
                        builder_.CreateICmpEQ(count, builder_.getInt32(static_cast<std::uint32_t>(word.size()))),
                        bytesBlock, otherwise);
                    builder_.SetInsertPoint(bytesBlock);
                    llvm::Value* same = builder_.getTrue();
                    for (std::size_t i = 0; i < word.size(); ++i)
                    {
                        llvm::Value* at = builder_.CreateConstInBoundsGEP1_32(builder_.getInt8Ty(), found,
                                                                               static_cast<unsigned>(i));
                        llvm::Value* byte = builder_.CreateLoad(builder_.getInt8Ty(), at);
                        same = builder_.CreateAnd(
                            same, builder_.CreateICmpEQ(byte, builder_.getInt8(static_cast<std::uint8_t>(word[i]))));
                    }
                    builder_.CreateCondBr(same, value ? isTrue : isFalse, otherwise);
                    builder_.SetInsertPoint(otherwise);
                }
                builder_.CreateBr(bad);
                for (const auto& [block, value] : {std::pair(isTrue, true), std::pair(isFalse, false)})
                {
                    builder_.SetInsertPoint(block);
                    tag(builder_.CreateStore(builder_.getInt1(value), slot), type);
                    builder_.CreateBr(done);
                }
            }
            builder_.SetInsertPoint(done);
        }
        if (const std::optional<std::pair<double, double>> range = rangeOf(param))
        {
            llvm::LoadInst* loaded = builder_.CreateLoad(typeOf(type), slot, param.name);
            tag(loaded, type);
            llvm::Type* real = builder_.getDoubleTy();
            llvm::Value* value = type == ast::Type::Float ? builder_.CreateFPExt(loaded, real)
                                 : type == ast::Type::Int ? builder_.CreateSIToFP(loaded, real)
                                                          : builder_.CreateUIToFP(loaded, real);
            llvm::Value* inside = builder_.CreateAnd(
                builder_.CreateFCmpOGE(value, llvm::ConstantFP::get(real, range->first)),
                builder_.CreateFCmpOLE(value, llvm::ConstantFP::get(real, range->second)), param.name + ".inrange");
            llvm::BasicBlock* fits = newBlock("param.fits");
            builder_.CreateCondBr(inside, fits, bad);
            builder_.SetInsertPoint(fits);
        }
        declare();
    }

    // `reply status, ...;`: the route's request answered, in the form its values say (one: http_reply; then a string
    // literal, or a string: http_reply_text; then a JSON literal; else four: http_reply_bytes), each value checked as
    // the builtin checks it; a refusal that leaves the request the route's (HTTP_LIMIT, HTTP_INVALID) answered 500;
    // then the deferred statements of the route's blocks, and the serve's next request.
    void Lowering::lowerReply(SourceLocation location, const ast::ReplyStmt& reply)
    {
        const Breakable* around =
            innermost({Breakable::Kind::Route, Breakable::Kind::Defer, Breakable::Kind::Region});
        if (around == nullptr)
        {
            error(location, "'reply' statement not in a route");
            return;
        }
        if (around->kind == Breakable::Kind::Defer)
        {
            error(location, "cannot reply from a defer statement");
            return;
        }
        if (around->kind == Breakable::Kind::Region)
        {
            error(location, regions_.back().loop
                                ? "'reply' cannot leave a parallel for: its iterations run at once, and none can end "
                                  "the route"
                                : "'reply' cannot leave a statement of a parallel block");
            return;
        }
        RouteState& route = routes_.back();
        ++route.replies;
        if (reply.args.empty())
        {
            builder_.CreateBr(route.next);   // the parser's error
            return;
        }
        const ast::Type requestType = ast::Type::handleType(ast::Handle::HttpRequest);
        llvm::LoadInst* request = builder_.CreateLoad(builder_.getPtrTy(), route.request, "request");
        tag(request, requestType);
        const std::size_t count = reply.args.size();
        const auto* json = count >= 2 ? std::get_if<ast::JsonExpr>(&reply.args[1]->node) : nullptr;
        Value answered{nullptr, ast::Type::Int, nullptr, true};
        if (json != nullptr && count > 2)
            error(begin(*reply.args[2]), "a JSON literal is a reply's whole body: nothing follows it");
        else if (json != nullptr)
            answered = lowerJsonReply(location, *reply.args[0], *json, request);
        else
        {
            // A string second, a literal or a value of type string, is the text form's, whose content type is a
            // literal only: a `string` there is the error a non-literal is, at it, and not the bytes form's three.
            const auto givesString = [&](const ast::Expr& expr) {
                if (isString(expr) || staticType(expr) == std::optional(ast::Type::String))
                    return true;
                const auto* call = std::get_if<ast::CallExpr>(&expr.node);
                const RuntimeBuiltin* builtin = call != nullptr ? findRuntimeBuiltin(call->callee) : nullptr;
                return builtin != nullptr && resultType(builtin->result) == ast::Type::String;
            };
            const auto* choice = count >= 2 ? std::get_if<ast::ConditionalExpr>(&reply.args[1]->node) : nullptr;
            const bool text = count >= 2
                              && (givesString(*reply.args[1])
                                  || (choice != nullptr
                                      && (givesString(*choice->thenValue) || givesString(*choice->elseValue))));
            if (count != 1 && (text ? count < 3 : count != 4))
                error(location, "no form of reply takes " + std::to_string(count) + " values: " + kReplyForms);
            else
            {
                std::vector<const ast::Expr*> args{nullptr};
                for (const ast::ExprPtr& arg : reply.args)
                    args.push_back(arg.get());
                const std::string_view name = count == 1 ? "http_reply" : text ? "http_reply_text" : "http_reply_bytes";
                answered = lowerRuntimeCall(location, *findRuntimeBuiltin(name), args, Value{request, requestType});
            }
        }
        if (!answered.failed)
        {
            // Refused, the request still the route's: answered all the same.
            llvm::Value* refused = builder_.CreateOr(
                builder_.CreateICmpEQ(answered.ir, builder_.getInt32(static_cast<std::uint32_t>(kWebLimit))),
                builder_.CreateICmpEQ(answered.ir, builder_.getInt32(static_cast<std::uint32_t>(kWebInvalid))),
                "reply.refused");
            llvm::BasicBlock* again = newBlock("reply.refused");
            llvm::BasicBlock* done = newBlock("reply.done");
            builder_.CreateCondBr(refused, again, done);
            builder_.SetInsertPoint(again);
            callRuntime("fminus_web_http_reply", builder_.getInt32Ty(), {request, builder_.getInt32(500)});
            builder_.CreateBr(done);
            builder_.SetInsertPoint(done);
        }
        runDefers(route.defers);
        builder_.CreateBr(route.next);
    }

    // `reply status, json { ... };`: the literal made into a format of the reply's and its values (jsonFormat),
    // answered as http_reply_text with the content type application/json. Failed when the status or the literal was in
    // error.
    Lowering::Value Lowering::lowerJsonReply(SourceLocation location, const ast::Expr& status, const ast::JsonExpr& json,
                                             llvm::Value* request)
    {
        (void)location;
        const Value code = isString(status) ? refuseString(status, ast::Type::Int, Use::Passing, begin(status))
                                            : lowerAs(status, ast::Type::Int);
        llvm::Value* converted = convert(code, ast::Type::Int, begin(status), Use::Passing);
        std::string format;
        std::vector<llvm::Value*> values;
        const bool fine = jsonFormat(json.value, format, values);
        if (!fine || code.failed)
            return {nullptr, ast::Type::Int, nullptr, true};
        const std::string type = "application/json";
        llvm::Value* answered = callRuntime(
            "fminus_web_http_reply_text", builder_.getInt32Ty(),
            {request, converted, stringConstant(ast::StringExpr{type, {}}),
             builder_.getInt32(static_cast<std::uint32_t>(type.size())), stringConstant(ast::StringExpr{format, {}}),
             builder_.getInt32(static_cast<std::uint32_t>(format.size())), replyValues(values),
             builder_.getInt32(static_cast<std::uint32_t>(values.size()))});
        answered->setName("http_reply_text");
        return {answered, ast::Type::Int};
    }

    // A JSON literal's value appended to `format`, a reply's format: an object's and an array's text, its keys
    // escaped, and a place for each value, its value (a double, as a reply's format takes) appended to `values`, in
    // the order written: %r a float, %R a double, %d an int, %b a bool. Each error once, where it is; false after one.
    bool Lowering::jsonFormat(const ast::JsonValue& value, std::string& format, std::vector<llvm::Value*>& values)
    {
        switch (value.kind)
        {
        case ast::JsonValue::Kind::Value:
        {
            const ast::Expr& expr = *value.value;
            // Lowered as a string is wanted (lowerAs), so that a literal, an arm of a ?: among them, is one: a string,
            // literal or not, is a later step's, one error at it.
            const Value lowered = lowerAs(expr, ast::Type::String);
            if (lowered.failed)
                return false;
            if (lowered.type == ast::Type::String)
            {
                error(begin(expr), "a JSON literal's value is a float, a double, an int, a bool, an object or an "
                                   "array in this version, and not yet a string");
                return false;
            }
            if (lowered.type.enumeration != 0)
            {
                error(begin(expr), "a JSON literal does not take a value of an enumerated type yet: cast it to "
                                   "int for its number");
                return false;
            }
            llvm::Type* real = builder_.getDoubleTy();
            if (lowered.type == ast::Type::Float)
            {
                format += "%r";
                values.push_back(builder_.CreateFPExt(lowered.ir, real, "json.float"));
            }
            else if (lowered.type == ast::Type::Double)
            {
                format += "%R";
                values.push_back(lowered.ir);
            }
            else if (lowered.type == ast::Type::Int)
            {
                format += "%d";
                values.push_back(builder_.CreateSIToFP(lowered.ir, real, "json.int"));
            }
            else if (lowered.type == ast::Type::Bool)
            {
                format += "%b";
                values.push_back(builder_.CreateUIToFP(lowered.ir, real, "json.bool"));
            }
            else
            {
                error(begin(expr), "a JSON literal's value is a float, a double, an int, a bool, an object or an "
                                   "array, not " + quoted(ast::typeName(lowered.type)));
                return false;
            }
            return true;
        }
        case ast::JsonValue::Kind::Object:
        {
            bool fine = true;
            format += '{';
            std::map<std::string, SourceLocation> seen;
            for (std::size_t i = 0; i < value.elements.size(); ++i)
            {
                const ast::StringExpr& key = value.keys[i];
                const SourceLocation where = value.keyLocations[i];
                if (const auto [previous, added] = seen.try_emplace(key.value, where); !added)
                {
                    diagnostics_.error(where, "duplicate key " + quoted(key.value) + " in a JSON object",
                                       {Diagnostic{previous->second, "previous key is here", {}}});
                    fine = false;
                }
                format += i > 0 ? ",\"" : "\"";
                if (const std::optional<std::size_t> bad = jsonKey(key.value, format))
                {
                    error(byteAt(key, *bad, where), "a JSON key is UTF-8 text, and this byte is no part of any");
                    fine = false;
                }
                format += "\":";
                fine = jsonFormat(value.elements[i], format, values) && fine;
            }
            format += '}';
            return fine;
        }
        case ast::JsonValue::Kind::Array:
        {
            bool fine = true;
            format += '[';
            for (std::size_t i = 0; i < value.elements.size(); ++i)
            {
                if (i > 0)
                    format += ',';
                fine = jsonFormat(value.elements[i], format, values) && fine;
            }
            format += ']';
            return fine;
        }
        }
        return false;
    }
} // namespace fminus::ir::detail
