// src/ir/Parallel.cpp: parallel for and parallel { }, FMinus's multithreading (extending.md, recipe 22). Structured
// fork and join, as OpenMP's parallel for and sections are, in two statements of the language:
//
//   parallel for (int i = first; i < end; ++i) body    the iterations run at once, spread over the machine's cores,
//                                                      and the loop ends when every one has run
//   parallel { left = l.tick(x); right = r.tick(y); }  so do the block's statements, each a task: the block ends when
//                                                      every one has run
//
// Each region's body becomes a function of its own, internal, of the runtime's task type (runtime/Runtime.h):
// `void @"<function>.parallel.<n>"(ptr %context, i32 %begin, i32 %end)`, which runs the iterations begin to end - 1, or
// for a block its statements begin to end - 1 (a switch on each one's number). Where the statement was, the function
// around it fills a context, a frame of its own, with what the body uses of it, and calls
// fminus_rt_parallel_for(body, context, begin, end, threads), which spreads the range over the pool's threads and its
// own and returns when all of it has run (runtime/Parallel.cpp). Iterations run in no particular order, on any thread,
// in chunks: a loop whose iterations only read what others write, and each write its own, computes what it would run
// in order, bit for bit.
//
// Captures. The body reaches a variable of the function around it through find(), which captures it: a variable the
// region only reads, by value, copied into the context where the statement starts and from there into a slot of the
// body's own, as a parameter is (mem2reg makes it a register); one it assigns or takes the address of, and an array
// or a module, by reference: its address. A method's `this` is a variable too. What a method reads of a parameter
// where it starts (atEntry: a knob's value, what a stream's reads use) the method reads, once, and hands the body by
// value: every iteration sees what the call does. A region in a region captures from the one around it, which
// captures in turn from its own surroundings.
//
// Races. Before it lowers a region, check() walks it (Walker, below) and refuses what would race, two threads at once
// on one variable with no order between them, where the program can be seen to do it:
//   - a parallel for that assigns a variable from outside it (or a field of `this`), or calls a method that changes
//     one, on it: every iteration would. What an iteration reaches through an index (out[i], voices[v].y) or a pointer
//     is its own to keep apart from the others', as in C with OpenMP: the compiler sees which variable, not which
//     element;
//   - a parallel for that assigns its own loop variable (each iteration has its own);
//   - two statements of a parallel { } of which one changes a variable that the other uses;
//   - a statement of a parallel { } that declares a variable (it would end with the statement);
//   - break or return out of a region, or continue out of a statement of a block (Statements.cpp).
// Whether a method changes its module (changesModule) is worked out once, over every method: one that assigns one of
// its fields (an array field's elements included) or takes the address of one, or of the module (`this` as a value),
// or calls a method that does on the module or on a module among its fields. What it writes through a pointer field
// is not its module's.
#include "ir/Lowering.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/ModRef.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <functional>
#include <set>

namespace fminus::ir::detail
{
    namespace
    {
        // Where an access lands: a variable from outside the region (Outer), a field of the method's module through
        // `this` (Self; no field at all is the whole module), or one of the region's own (Local); the fields followed
        // from it (m.y.z is m, then y and z), and whether an index was too (buffer[i], voices[v].y): an element, which
        // only the program knows apart from the other iterations'.
        struct Place
        {
            enum class Root
            {
                Outer,
                Self,
                Local,
            };
            Root root = Root::Local;
            std::string name;   // Outer's and Local's: the variable
            std::vector<std::string> fields;
            bool element = false;
            ast::Type type = ast::Type::Float;   // what is there
            bool counter = false;                // a parallel for's own loop variable
        };

        struct Access
        {
            enum class Kind
            {
                Read,
                Write,     // an assignment, ++ or --
                Address,   // &x, an array's name as a pointer, `this` as a value: what the pointer reaches is unknown
                Call,      // a method called on it: a write when the method changes its module
            };
            Kind kind = Kind::Read;
            Place place;
            SourceLocation location;
            std::string method;   // a call's, qualified: "OnePole::tick"
        };

        // What a walk needs of the code around what it walks.
        struct Surroundings
        {
            const ast::Module* module = nullptr;                                 // a method's, for its fields
            std::function<std::optional<ast::Type>(const std::string&)> outer;   // a variable from outside: its type
            std::function<bool(const std::string&)> defined;                     // whether the program has a function
        };

        const ast::Field* fieldOf(const ast::Module& module, const std::string& name)
        {
            for (const ast::Field& field : module.fields)
            {
                if (field.name == name)
                    return field.isParam ? nullptr : &field;   // a parameter is read as cutoff[i], and never written
            }
            return nullptr;
        }

        // Calls f on each of `expr`'s operands, in order.
        template <typename F>
        void operands(const ast::Expr& expr, F&& f)
        {
            std::visit(Overloaded{
                           [](const ast::NumberExpr&) {},
                           [](const ast::VariableExpr&) {},
                           [](const ast::BoolExpr&) {},
                           [&](const ast::UnaryExpr& unary) { f(*unary.operand); },
                           [&](const ast::BinaryExpr& binary) {
                               f(*binary.lhs);
                               f(*binary.rhs);
                           },
                           [&](const ast::LogicalExpr& logical) {
                               f(*logical.lhs);
                               f(*logical.rhs);
                           },
                           [&](const ast::CallExpr& call) {
                               for (const ast::ExprPtr& arg : call.args)
                                   f(*arg);
                           },
                           [&](const ast::AssignExpr& assign) {
                               f(*assign.target);
                               f(*assign.value);
                           },
                           [&](const ast::ConditionalExpr& conditional) {
                               f(*conditional.condition);
                               f(*conditional.thenValue);
                               f(*conditional.elseValue);
                           },
                           [&](const ast::ConstructExpr& construct) {
                               for (const ast::ExprPtr& arg : construct.args)
                                   f(*arg);
                           },
                           [&](const ast::MemberExpr& member) { f(*member.base); },
                           [&](const ast::MethodCallExpr& call) {
                               f(*call.object);
                               for (const ast::ExprPtr& arg : call.args)
                                   f(*arg);
                           },
                           [](const ast::ThisExpr&) {},
                           [&](const ast::IncrementExpr& increment) { f(*increment.target); },
                           [&](const ast::CastExpr& cast) { f(*cast.operand); },
                           [&](const ast::IndexExpr& index) {
                               f(*index.base);
                               f(*index.index);
                           },
                           [](const ast::SizeofExpr&) {},
                       },
                       expr.node);
        }

        // Whether `expr` names `name` anywhere.
        bool mentions(const ast::Expr& expr, const std::string& name)
        {
            if (const auto* variable = std::get_if<ast::VariableExpr>(&expr.node); variable != nullptr)
                return variable->name == name;
            bool found = false;
            operands(expr, [&](const ast::Expr& operand) { found = found || mentions(operand, name); });
            return found;
        }

        bool isVariable(const ast::Expr& expr, const std::string& name)
        {
            const auto* variable = std::get_if<ast::VariableExpr>(&expr.node);
            return variable != nullptr && variable->name == name;
        }

        // Every access some code makes to what it did not declare itself, in the order it makes them.
        class Walker
        {
        public:
            explicit Walker(Surroundings surroundings) : around_(std::move(surroundings))
            {
                scopes_.emplace_back();
            }

            // A variable of the code's own: a parameter, or the loop variable (`counter`, whose writes are recorded).
            void declare(const std::string& name, ast::Type type, bool counter = false)
            {
                scopes_.back()[name] = Local{type, counter};
            }

            const std::vector<Access>& accesses() const
            {
                return accesses_;
            }

            void statement(const ast::Stmt& stmt)
            {
                std::visit(Overloaded{
                               [&](const ast::DeclStmt& decl) {
                                   if (decl.init)
                                       expression(*decl.init, Access::Kind::Read);
                                   for (const ast::ExprPtr& element : decl.list)
                                       expression(*element, Access::Kind::Read);
                                   declare(decl.name, decl.type);
                               },
                               [&](const ast::ExprStmt& statement) { expression(*statement.expr, Access::Kind::Read); },
                               [&](const ast::ReturnStmt& ret) {
                                   if (ret.value)
                                       expression(*ret.value, Access::Kind::Read);
                               },
                               [&](const ast::BlockStmt& block) {
                                   scoped([&] {
                                       for (const ast::Stmt& item : block.items)
                                           statement(item);
                                   });
                               },
                               [](const ast::EmptyStmt&) {},
                               [&](const ast::IfStmt& branch) {
                                   expression(*branch.condition, Access::Kind::Read);
                                   scoped([&] { statement(*branch.thenBranch); });
                                   if (branch.elseBranch)
                                       scoped([&] { statement(*branch.elseBranch); });
                               },
                               [&](const ast::WhileStmt& loop) {
                                   expression(*loop.condition, Access::Kind::Read);
                                   scoped([&] { statement(*loop.body); });
                               },
                               [&](const ast::ForStmt& loop) { forLoop(loop.init, loop.condition, loop.step, *loop.body); },
                               [](const ast::BreakStmt&) {},
                               [](const ast::ContinueStmt&) {},
                               [&](const ast::ParallelForStmt& loop) {
                                   forLoop(loop.init, loop.condition, loop.step, *loop.body);
                               },
                               [&](const ast::ParallelBlockStmt& block) {
                                   for (const ast::Stmt& task : block.tasks)
                                       scoped([&] { statement(task); });
                               },
                           },
                           stmt.node);
            }

            // `expr` evaluated, its value used as `use` says.
            void expression(const ast::Expr& expr, Access::Kind use)
            {
                std::visit(Overloaded{
                               [&](const ast::VariableExpr&) { placed(expr, use); },
                               [&](const ast::MemberExpr&) { placed(expr, use); },
                               [&](const ast::IndexExpr&) { placed(expr, use); },
                               [&](const ast::UnaryExpr& unary) {
                                   if (unary.op == ast::UnaryOp::Dereference)
                                       placed(expr, use);
                                   else if (unary.op == ast::UnaryOp::AddressOf)
                                       placed(*unary.operand, Access::Kind::Address);
                                   else
                                       expression(*unary.operand, Access::Kind::Read);
                               },
                               [&](const ast::AssignExpr& assign) {
                                   expression(*assign.value, Access::Kind::Read);
                                   placed(*assign.target, Access::Kind::Write);
                               },
                               [&](const ast::IncrementExpr& increment) {
                                   placed(*increment.target, Access::Kind::Write);
                               },
                               [&](const ast::CallExpr& call) {
                                   for (const ast::ExprPtr& arg : call.args)
                                       expression(*arg, Access::Kind::Read);
                                   // In a method, tick(x) is this->tick(x) when the module has a tick (Calls.cpp).
                                   if (around_.module != nullptr && around_.defined(around_.module->name + "::" + call.callee))
                                       record(Access::Kind::Call, self(), expr.location,
                                              around_.module->name + "::" + call.callee);
                               },
                               [&](const ast::MethodCallExpr& call) {
                                   for (const ast::ExprPtr& arg : call.args)
                                       expression(*arg, Access::Kind::Read);
                                   if (call.arrow)
                                   {
                                       // this->tick() is this's; p->tick() is through a pointer, the program's.
                                       if (std::holds_alternative<ast::ThisExpr>(call.object->node) && around_.module != nullptr)
                                           record(Access::Kind::Call, self(), expr.location,
                                                  around_.module->name + "::" + call.method);
                                       else
                                           expression(*call.object, Access::Kind::Read);
                                       return;
                                   }
                                   const std::optional<Place> object = place(*call.object);
                                   if (object && ast::isModule(object->type))
                                       record(Access::Kind::Call, *object, expr.location,
                                              object->type.module->name + "::" + call.method);
                               },
                               [&](const ast::ThisExpr&) { record(Access::Kind::Address, self(), expr.location); },
                               [&](const ast::NumberExpr&) {},
                               [&](const ast::BoolExpr&) {},
                               [&](const ast::SizeofExpr&) {},
                               [&](const ast::BinaryExpr&) { reads(expr); },
                               [&](const ast::LogicalExpr&) { reads(expr); },
                               [&](const ast::ConditionalExpr&) { reads(expr); },
                               [&](const ast::ConstructExpr&) { reads(expr); },
                               [&](const ast::CastExpr&) { reads(expr); },
                           },
                           expr.node);
            }

        private:
            struct Local
            {
                ast::Type type;
                bool counter = false;
            };

            template <typename F>
            void scoped(F&& f)
            {
                scopes_.emplace_back();
                f();
                scopes_.pop_back();
            }

            void forLoop(const std::vector<ast::Stmt>& init, const ast::ExprPtr& condition, const ast::ExprPtr& step,
                         const ast::Stmt& body)
            {
                scoped([&] {
                    for (const ast::Stmt& item : init)
                        statement(item);
                    if (condition)
                        expression(*condition, Access::Kind::Read);
                    if (step)
                        expression(*step, Access::Kind::Read);
                    scoped([&] { statement(body); });
                });
            }

            void reads(const ast::Expr& expr)
            {
                operands(expr, [&](const ast::Expr& operand) { expression(operand, Access::Kind::Read); });
            }

            Place self() const
            {
                return Place{Place::Root::Self, "", {}, false, ast::Type::moduleType(around_.module), false};
            }

            // The place `expr` designates, an lvalue's, with what evaluating it reads recorded on the way (an index); or
            // nothing, for what designates no variable, which is then evaluated as a read.
            std::optional<Place> place(const ast::Expr& expr)
            {
                if (const auto* variable = std::get_if<ast::VariableExpr>(&expr.node))
                {
                    for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope)
                    {
                        if (const auto found = scope->find(variable->name); found != scope->end())
                            return Place{Place::Root::Local, variable->name, {}, false, found->second.type,
                                         found->second.counter};
                    }
                    if (around_.outer)
                    {
                        if (const std::optional<ast::Type> type = around_.outer(variable->name))
                            return Place{Place::Root::Outer, variable->name, {}, false, *type, false};
                    }
                    if (around_.module != nullptr)
                    {
                        if (const ast::Field* field = fieldOf(*around_.module, variable->name))
                            return Place{Place::Root::Self, "", {variable->name}, false, field->type, false};
                    }
                    return std::nullopt;   // a parameter, read as cutoff[i]; or an error of its own
                }
                if (const auto* member = std::get_if<ast::MemberExpr>(&expr.node))
                {
                    if (member->arrow)
                    {
                        if (std::holds_alternative<ast::ThisExpr>(member->base->node) && around_.module != nullptr)
                        {
                            if (const ast::Field* field = fieldOf(*around_.module, member->name))
                                return Place{Place::Root::Self, "", {member->name}, false, field->type, false};
                            return std::nullopt;
                        }
                        expression(*member->base, Access::Kind::Read);   // p->y: through a pointer, the program's
                        return std::nullopt;
                    }
                    std::optional<Place> base = place(*member->base);
                    if (!base)
                        return std::nullopt;
                    if (ast::isModule(base->type))
                    {
                        const ast::Field* field = fieldOf(*base->type.module, member->name);
                        if (field == nullptr)
                            return std::nullopt;   // an error of its own
                        base->fields.push_back(member->name);
                        base->type = field->type;
                    }
                    return base;   // or a vector's components: its whole
                }
                if (const auto* index = std::get_if<ast::IndexExpr>(&expr.node))
                {
                    expression(*index->index, Access::Kind::Read);
                    std::optional<Place> base = place(*index->base);
                    if (base && ast::isArray(base->type))
                    {
                        base->element = true;
                        base->type = ast::pointee(base->type);
                        return base;
                    }
                    if (base)
                        record(Access::Kind::Read, *base, index->base->location);   // a pointer, read
                    return std::nullopt;
                }
                if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
                    unary != nullptr && unary->op == ast::UnaryOp::Dereference)
                {
                    expression(*unary->operand, Access::Kind::Read);
                    return std::nullopt;
                }
                expression(expr, Access::Kind::Read);
                return std::nullopt;
            }

            // `expr`, a place, used as `use`. An array read is its address (C's decay: a pointer to it escapes).
            void placed(const ast::Expr& expr, Access::Kind use)
            {
                if (std::optional<Place> found = place(expr))
                {
                    if (use == Access::Kind::Read && ast::isArray(found->type))
                        use = Access::Kind::Address;
                    record(use, std::move(*found), expr.location);
                }
            }

            void record(Access::Kind kind, Place place, SourceLocation location, std::string method = {})
            {
                if (place.root == Place::Root::Local && !(place.counter && kind == Access::Kind::Write))
                    return;   // the code's own
                accesses_.push_back(Access{kind, std::move(place), location, std::move(method)});
            }

            Surroundings around_;
            std::vector<std::unordered_map<std::string, Local>> scopes_;
            std::vector<Access> accesses_;
        };

        // A place as the program writes it: m.y, y (a field), this.
        std::string describe(const Place& place)
        {
            std::string text = place.root == Place::Root::Self ? "" : place.name;
            for (const std::string& field : place.fields)
                text += (text.empty() ? "" : ".") + field;
            return text.empty() ? "this" : text;
        }

        // Whether two places can be the same memory: one variable (or `this`), and one's fields the start of the
        // other's (m and m.y are, m.x and m.y are not).
        bool overlaps(const Place& a, const Place& b)
        {
            if (a.root != b.root || (a.root == Place::Root::Outer && a.name != b.name))
                return false;
            const std::size_t common = std::min(a.fields.size(), b.fields.size());
            return std::equal(a.fields.begin(), a.fields.begin() + static_cast<std::ptrdiff_t>(common),
                              b.fields.begin());
        }
    } // namespace

    // Whether `method` (qualified: "OnePole::tick") may change the module it is called on (the top of this file).
    // Worked out for every method at once, the first time a region asks: each one's own accesses, then, until nothing
    // changes, those whose calls on the module (or on a module among its fields) reach one that changes it.
    bool Lowering::changesModule(const std::string& method)
    {
        if (!changes_)
        {
            std::unordered_map<std::string, bool> changes;
            std::unordered_map<std::string, std::vector<std::string>> calls;
            for (const auto& [name, function] : functions_)
            {
                if (function->module == nullptr || !function->isDefinition)
                    continue;
                Surroundings around;
                around.module = function->module;
                around.defined = [this](const std::string& callee) { return functions_.contains(callee); };
                Walker walker(std::move(around));
                for (const ast::Param& param : function->params)
                    walker.declare(param.name, param.type);
                for (const ast::Stmt& stmt : function->body)
                    walker.statement(stmt);
                bool changed = false;
                for (const Access& access : walker.accesses())
                {
                    if (access.place.root != Place::Root::Self)
                        continue;
                    if (access.kind == Access::Kind::Write || access.kind == Access::Kind::Address)
                        changed = true;
                    else if (access.kind == Access::Kind::Call)
                        calls[name].push_back(access.method);
                }
                changes[name] = changed;
            }
            for (bool more = true; more;)
            {
                more = false;
                for (auto& [name, changed] : changes)
                {
                    for (const std::string& callee : calls[name])
                    {
                        const auto found = changes.find(callee);
                        if (changed || found == changes.end() || !found->second)
                            continue;
                        changed = true;
                        more = true;
                    }
                }
            }
            changes_ = std::move(changes);
        }
        const auto found = changes_->find(method);
        return found != changes_->end() && found->second;
    }

    // What the region's code can see around it: the variables of the scopes it starts in, and a method's fields.
    namespace
    {
        Surroundings regionSurroundings(const ast::Function* function,
                                        std::function<std::optional<ast::Type>(const std::string&)> outer,
                                        std::function<bool(const std::string&)> defined)
        {
            Surroundings around;
            around.module = function != nullptr ? function->module : nullptr;
            around.outer = std::move(outer);
            around.defined = std::move(defined);
            return around;
        }
    } // namespace

    // A parallel for's races (the top of this file), reported; the variables from outside that it reaches by address.
    std::unordered_set<std::string> Lowering::checkParallelFor(const ast::ParallelForStmt& loop,
                                                               const std::string& counter)
    {
        Walker walker(regionSurroundings(
            function_,
            [this](const std::string& name) -> std::optional<ast::Type> {
                const Binding* binding = lookup(name);
                return binding != nullptr ? std::optional<ast::Type>(binding->type) : std::nullopt;
            },
            [this](const std::string& name) { return functions_.contains(name); }));
        if (!counter.empty())
            walker.declare(counter, ast::Type::Int, true);
        walker.statement(*loop.body);

        std::unordered_set<std::string> byReference;
        std::set<std::string> reported;
        for (const Access& access : walker.accesses())
        {
            if (access.place.counter)
            {
                error(access.location, "cannot assign " + quoted(access.place.name)
                                           + ", the loop variable of a parallel for: each iteration has its own");
                continue;
            }
            const bool writes = access.kind == Access::Kind::Write
                                || (access.kind == Access::Kind::Call && changesModule(access.method));
            if (access.place.root == Place::Root::Outer && (writes || access.kind == Access::Kind::Address))
                byReference.insert(access.place.name);
            if (!writes || access.place.element || !reported.insert(describe(access.place)).second)
                continue;
            if (access.kind == Access::Kind::Call && access.place.root == Place::Root::Self && access.place.fields.empty())
                error(access.location, "every iteration of the parallel for calls " + quoted(access.method)
                                           + ", which changes the module it is called on ('this'), and the iterations "
                                             "run at once");
            else if (access.kind == Access::Kind::Call)
                error(access.location, "every iteration of the parallel for calls " + quoted(access.method) + " on "
                                           + quoted(describe(access.place))
                                           + ", which changes it, and the iterations run at once: give each iteration "
                                             "a module of its own (an array of them, indexed by the loop variable)");
            else
                error(access.location, "every iteration of the parallel for assigns " + quoted(describe(access.place))
                                           + ", and the iterations run at once: give each an element of its own (an "
                                             "array indexed by the loop variable), or combine them after the loop");
        }
        return byReference;
    }

    // A parallel { }'s races and declarations (the top of this file), reported; the variables from outside that it
    // reaches by address.
    std::unordered_set<std::string> Lowering::checkParallelBlock(const ast::ParallelBlockStmt& block)
    {
        std::vector<std::vector<Access>> tasks;
        for (const ast::Stmt& task : block.tasks)
        {
            if (const auto* decl = std::get_if<ast::DeclStmt>(&task.node))
                error(task.location, "a statement of a parallel block cannot declare a variable (" + quoted(decl->name)
                                         + " would end with it): declare it before the block, and assign it in one of "
                                           "the block's statements");
            Walker walker(regionSurroundings(
                function_,
                [this](const std::string& name) -> std::optional<ast::Type> {
                    const Binding* binding = lookup(name);
                    return binding != nullptr ? std::optional<ast::Type>(binding->type) : std::nullopt;
                },
                [this](const std::string& name) { return functions_.contains(name); }));
            walker.statement(task);
            tasks.push_back(walker.accesses());
        }

        // A statement's write: an assignment, an address taken (what the pointer reaches is unknown), a call that
        // changes its module. What a statement reaches through an index is its own to keep apart.
        const auto changes = [&](const Access& access) {
            return access.kind == Access::Kind::Write || access.kind == Access::Kind::Address
                   || (access.kind == Access::Kind::Call && changesModule(access.method));
        };
        std::unordered_set<std::string> byReference;
        for (const std::vector<Access>& task : tasks)
        {
            for (const Access& access : task)
            {
                if (access.place.root == Place::Root::Outer && changes(access))
                    byReference.insert(access.place.name);
            }
        }
        std::set<std::string> reported;
        for (std::size_t later = 1; later < tasks.size(); ++later)
        {
            for (std::size_t earlier = 0; earlier < later; ++earlier)
            {
                for (const Access& second : tasks[later])
                {
                    for (const Access& first : tasks[earlier])
                    {
                        const bool firstChanges = changes(first);
                        const bool secondChanges = changes(second);
                        if ((!firstChanges && !secondChanges) || first.place.element || second.place.element
                            || !overlaps(first.place, second.place))
                            continue;
                        const std::string what = describe(first.place.fields.size() <= second.place.fields.size()
                                                              ? first.place
                                                              : second.place);
                        if (!reported.insert(what).second)
                            continue;
                        const std::string message =
                            "the statements of a parallel block run at once: this one "
                            + std::string(secondChanges ? "changes " : "uses ") + quoted(what) + ", which "
                            + (firstChanges ? "another changes" : "another uses")
                            + (secondChanges && firstChanges ? " too" : "");
                        diagnostics_.error(second.location, message,
                                           {{first.location,
                                             quoted(what) + (firstChanges ? " is changed here" : " is used here"),
                                             {}}});
                    }
                }
            }
        }
        return byReference;
    }

    // `parallel for (int i = first; i < end; ++i) body`: the form checked, the bounds computed here, the body
    // outlined, and the iterations from first to end - 1 handed to the runtime.
    void Lowering::lowerParallelFor(SourceLocation location, const ast::ParallelForStmt& loop)
    {
        const char* form = "parallel for (int i = first; i < end; ++i)";
        const ast::DeclStmt* counter =
            loop.init.size() == 1 ? std::get_if<ast::DeclStmt>(&loop.init[0].node) : nullptr;
        const std::string name = counter != nullptr ? counter->name : "";
        if (counter == nullptr || counter->init == nullptr || counter->hasList)
            error(location, std::string("a parallel for declares the int it counts with, and where it starts: ") + form);
        else if (counter->type != ast::Type::Int)
            error(loop.init[0].location, "the loop variable of a parallel for is an int, not "
                                             + quoted(ast::typeName(counter->type)) + ": " + form);
        const auto* test = loop.condition ? std::get_if<ast::BinaryExpr>(&loop.condition->node) : nullptr;
        const bool bounded = test != nullptr && !name.empty() && isVariable(*test->lhs, name)
                             && (test->op == ast::BinaryOp::Less || test->op == ast::BinaryOp::LessEqual);
        if (!bounded && !name.empty())
            error(loop.condition ? loop.condition->location : location,
                  "the condition of a parallel for is where it ends, " + quoted(name + " < end") + " or "
                      + quoted(name + " <= end") + ": " + form);
        bool stepsByOne = false;
        if (loop.step != nullptr && !name.empty())
        {
            if (const auto* increment = std::get_if<ast::IncrementExpr>(&loop.step->node))
                stepsByOne = isVariable(*increment->target, name)
                             && (increment->op == ast::IncrementOp::PreIncrement
                                 || increment->op == ast::IncrementOp::PostIncrement);
            else if (const auto* assign = std::get_if<ast::AssignExpr>(&loop.step->node))
            {
                const auto* one = std::get_if<ast::NumberExpr>(&assign->value->node);
                stepsByOne = assign->op == ast::AssignOp::Add && isVariable(*assign->target, name) && one != nullptr
                             && one->spelling == "1";
            }
        }
        if (!stepsByOne && !name.empty())
            error(loop.step ? loop.step->location : location,
                  "a parallel for counts up by one: its step is " + quoted("++" + name) + ", " + quoted(name + "++")
                      + " or " + quoted(name + " += 1"));
        const bool selfBound = bounded && mentions(*test->rhs, name);
        if (selfBound)
            error(test->rhs->location, "the end of a parallel for is computed once, before the loop: it cannot use "
                                           + quoted(name));

        // The bounds, here, once: the first as the declaration's initialiser (read before the name is bound, as C
        // reads it), then the end.
        llvm::Value* begin = builder_.getInt32(0);
        llvm::Value* end = builder_.getInt32(0);
        if (counter != nullptr && counter->init != nullptr)
            begin = convert(lower(*counter->init), ast::Type::Int, counter->init->location, Use::Initializing);
        if (bounded && !selfBound)
        {
            const Value last = lower(*test->rhs);
            if (last.type != ast::Type::Int && last.type != ast::Type::Bool && ast::isArithmetic(last.type))
                error(test->rhs->location, "the end of a parallel for is an int, not " + quoted(ast::typeName(last.type)));
            end = convert(last, ast::Type::Int, test->rhs->location, Use::Initializing);
            if (test->op == ast::BinaryOp::LessEqual)
                end = builder_.CreateNSWAdd(end, builder_.getInt32(1), "end");
        }

        beginRegion(location, true, checkParallelFor(loop, name));
        // The body's iterations: for (int i = begin; i < end; ++i) body. continue goes to the step; break and return
        // are errors (Statements.cpp).
        llvm::Function* body = regions_.back().function;
        llvm::Type* i32 = builder_.getInt32Ty();
        llvm::AllocaInst* slot = allocate(i32, name.empty() ? "i" : name);
        builder_.CreateStore(body->getArg(1), slot);
        if (!name.empty())
            bind(name, Binding{slot, counter->isConst, ast::Type::Int});
        llvm::BasicBlock* condBlock = newBlock("parallel.cond");
        llvm::BasicBlock* bodyBlock = newBlock("parallel.body");
        llvm::BasicBlock* incBlock = newBlock("parallel.inc");
        llvm::BasicBlock* endBlock = newBlock("parallel.end");
        const llvm::Instruction* entry = builder_.CreateBr(condBlock);
        builder_.SetInsertPoint(condBlock);
        llvm::Value* at = builder_.CreateLoad(i32, slot, name);
        builder_.CreateCondBr(builder_.CreateICmpSLT(at, body->getArg(2), "parallel.more"), bodyBlock, endBlock);
        builder_.SetInsertPoint(bodyBlock);
        loops_.push_back({nullptr, incBlock});
        lowerScoped(*loop.body);
        loops_.pop_back();
        fallThrough(incBlock);
        builder_.SetInsertPoint(incBlock);
        builder_.CreateStore(builder_.CreateNSWAdd(builder_.CreateLoad(i32, slot, name), builder_.getInt32(1), "inc"),
                             slot);
        builder_.CreateBr(condBlock);
        mustProgress(condBlock, entry);
        builder_.SetInsertPoint(endBlock);
        endRegion(begin, end);
    }

    // `parallel { ... }`: its statements, task 0 to n - 1, in one outlined body that runs the tasks begin to end - 1,
    // each a case of a switch.
    void Lowering::lowerParallelBlock(SourceLocation location, const ast::ParallelBlockStmt& block)
    {
        std::unordered_set<std::string> byReference = checkParallelBlock(block);
        if (block.tasks.empty())
            return;
        beginRegion(location, false, std::move(byReference));
        llvm::Function* body = regions_.back().function;
        llvm::Type* i32 = builder_.getInt32Ty();
        llvm::AllocaInst* slot = allocate(i32, "task");
        builder_.CreateStore(body->getArg(1), slot);
        llvm::BasicBlock* condBlock = newBlock("parallel.cond");
        llvm::BasicBlock* switchBlock = newBlock("parallel.task");
        llvm::BasicBlock* incBlock = newBlock("parallel.inc");
        llvm::BasicBlock* endBlock = newBlock("parallel.end");
        const llvm::Instruction* entry = builder_.CreateBr(condBlock);
        builder_.SetInsertPoint(condBlock);
        builder_.CreateCondBr(builder_.CreateICmpSLT(builder_.CreateLoad(i32, slot, "task"), body->getArg(2), "parallel.more"),
                              switchBlock, endBlock);
        builder_.SetInsertPoint(switchBlock);
        llvm::SwitchInst* which = builder_.CreateSwitch(builder_.CreateLoad(i32, slot, "task"), incBlock,
                                                        static_cast<unsigned>(block.tasks.size()));
        for (std::size_t i = 0; i < block.tasks.size(); ++i)
        {
            llvm::BasicBlock* task = newBlock("parallel.statement");
            which->addCase(builder_.getInt32(static_cast<std::uint32_t>(i)), task);
            builder_.SetInsertPoint(task);
            loops_.push_back({nullptr, nullptr});
            lowerScoped(block.tasks[i]);
            loops_.pop_back();
            fallThrough(incBlock);
        }
        builder_.SetInsertPoint(incBlock);
        builder_.CreateStore(builder_.CreateNSWAdd(builder_.CreateLoad(i32, slot, "task"), builder_.getInt32(1), "inc"),
                             slot);
        builder_.CreateBr(condBlock);
        mustProgress(condBlock, entry);
        builder_.SetInsertPoint(endBlock);
        endRegion(builder_.getInt32(0), builder_.getInt32(static_cast<std::uint32_t>(block.tasks.size())));
    }

    // The region's function, `<function>.parallel.<n>`, made and entered: the region's code goes there from now on,
    // in scopes of its own, with no parameter read yet.
    void Lowering::beginRegion(SourceLocation location, bool loop, std::unordered_set<std::string> byReference)
    {
        llvm::LLVMContext& context = module_.getContext();
        llvm::Function* around = builder_.GetInsertBlock()->getParent();
        const std::string name =
            around->getName().str() + ".parallel." + std::to_string(++regionCount_[around->getName().str()]);
        llvm::Type* i32 = builder_.getInt32Ty();
        llvm::FunctionType* type = llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy(), i32, i32}, false);
        llvm::Function* function = llvm::Function::Create(type, llvm::GlobalValue::InternalLinkage, name, module_);
        function->getArg(0)->setName("context");
        function->getArg(1)->setName("begin");
        function->getArg(2)->setName("end");
        // It only reads its context, which nothing else writes while it runs, and keeps no pointer to it.
        function->addParamAttr(0, llvm::Attribute::NoAlias);
        function->addParamAttr(0, llvm::Attribute::ReadOnly);
        function->addParamAttr(0, llvm::Attribute::getWithCaptureInfo(context, llvm::CaptureInfo::none()));
        function->setDoesNotThrow();
        outlined_.push_back(function);

        Region region;
        region.location = location;
        region.loop = loop;
        region.base = scopes_.size();
        region.function = function;
        region.byReference = std::move(byReference);
        region.resume = builder_.saveIP();
        region.entryLoads.swap(entryLoads_);
        region.flatStreams.swap(flatStreams_);
        region.streamReads.swap(streamReads_);
        llvm::BasicBlock::Create(context, "entry", function);   // its slots, and the captures
        region.body = llvm::BasicBlock::Create(context, "start", function);
        builder_.SetInsertPoint(region.body);
        regions_.push_back(std::move(region));
        scopes_.emplace_back();   // the region's outermost: its captures, and a loop's variable
    }

    // The region's function finished, and the call of the runtime that runs it, where the region was: its context
    // filled first, from the function around it.
    void Lowering::endRegion(llvm::Value* begin, llvm::Value* end)
    {
        Region region = std::move(regions_.back());
        regions_.pop_back();
        llvm::Function* function = region.function;
        if (!builder_.GetInsertBlock()->hasTerminator())
            builder_.CreateRetVoid();
        llvm::IRBuilder<>(&function->getEntryBlock()).CreateBr(region.body);   // the captures done, the code
        if (!streamReads_.empty())
            scopeStreams(*function);
        std::string problem;
        llvm::raw_string_ostream os(problem);
        if (llvm::verifyFunction(*function, &os))
            error({1, 1}, "internal error: the IR of " + quoted(function->getName()) + " is invalid: " + problem);

        // Back in the function around it.
        scopes_.resize(region.base);
        entryLoads_ = std::move(region.entryLoads);
        flatStreams_ = std::move(region.flatStreams);
        streamReads_ = std::move(region.streamReads);
        builder_.restoreIP(region.resume);

        llvm::Value* context = llvm::ConstantPointerNull::get(builder_.getPtrTy());
        if (!region.captures.empty())
        {
            llvm::AllocaInst* frame = allocate(llvm::ArrayType::get(builder_.getInt8Ty(), region.size),
                                               function->getName().str() + ".context");
            frame->setAlignment(region.align);
            for (const Capture& capture : region.captures)
            {
                llvm::Value* value = outerValue(capture);
                llvm::Value* at = builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), frame, capture.offset);
                builder_.CreateAlignedStore(value, at, module_.getDataLayout().getABITypeAlign(value->getType()));
            }
            context = frame;
        }
        // The runtime's name is its own (as the math builtins' are, Math.cpp): a program's function of that name would
        // take the call.
        if (const auto found = functions_.find("fminus_rt_parallel_for"); found != functions_.end())
            diagnostics_.error(region.location,
                               "'parallel' reserves 'fminus_rt_parallel_for' for the runtime's function, and this "
                               "program declares a function of that name: rename it",
                               {Diagnostic{found->second->location, "'fminus_rt_parallel_for' declared here", {}}});
        callRuntime("fminus_rt_parallel_for", builder_.getVoidTy(),
                    {function, context, begin, end, builder_.getInt32(threads_)});
    }

    // A variable of the function around the region, captured: by value into a slot of the region's own, or by
    // reference, its address (the top of this file). The binding goes into the region's outermost scope, where the
    // next look finds it.
    const Lowering::Binding* Lowering::capture(const std::string& name, const Binding& outer)
    {
        Region& region = regions_.back();
        const bool reference =
            ast::isArray(outer.type) || ast::isModule(outer.type) || region.byReference.contains(name);
        Binding binding{nullptr, outer.isConst, outer.type};
        if (reference)
            binding.slot = captured({Capture::Kind::Reference, name}, builder_.getPtrTy(), name + ".ref");
        else
        {
            llvm::Value* value = captured({Capture::Kind::Value, name}, typeOf(outer.type), name);
            llvm::AllocaInst* slot = allocate(typeOf(outer.type), name + ".addr");
            llvm::IRBuilder<> at(&region.function->getEntryBlock());
            at.CreateStore(value, slot);
            binding.slot = slot;
        }
        return &scopes_[region.base].emplace(name, binding).first->second;
    }

    // The next place in the region's context, for `capture`, and its load where the region's function starts.
    llvm::Value* Lowering::captured(Capture capture, llvm::Type* type, const std::string& name)
    {
        Region& region = regions_.back();
        const llvm::DataLayout& layout = module_.getDataLayout();
        const llvm::Align align = layout.getABITypeAlign(type);
        capture.offset = llvm::alignTo(region.size, align);
        region.size = capture.offset + layout.getTypeAllocSize(type).getFixedValue();
        region.align = std::max(region.align, align);
        region.captures.push_back(std::move(capture));
        llvm::IRBuilder<> at(&region.function->getEntryBlock());   // after its slots and the captures before
        llvm::Value* address = at.CreateConstInBoundsGEP1_64(at.getInt8Ty(), region.function->getArg(0),
                                                            region.captures.back().offset, name + ".at");
        return at.CreateAlignedLoad(type, address, align, name);
    }

    // What the method read of parameter `field` where it starts (atEntry), for the region: a knob's value, or a
    // stream's flat test, value, base and stride (flatStreams_, the region's own), each captured by value.
    llvm::Value* Lowering::capturedParameter(unsigned field, bool stream)
    {
        const ast::Field& parameter = function_->module->fields[field];
        llvm::Value* result = nullptr;
        if (!stream)
        {
            llvm::Type* type = parameter.type == ast::Type::Bool ? builder_.getInt1Ty() : slotTypeOf(parameter.type);
            result = captured({Capture::Kind::Parameter, parameter.name, field, false, 0}, type, parameter.name);
        }
        else
        {
            llvm::Value* flat = captured({Capture::Kind::Parameter, parameter.name, field, true, 0},
                                         builder_.getInt1Ty(), parameter.name + ".flat");
            llvm::Value* value = captured({Capture::Kind::Parameter, parameter.name, field, true, 1},
                                          slotTypeOf(parameter.type), parameter.name + ".value");
            llvm::Value* base = captured({Capture::Kind::Parameter, parameter.name, field, true, 2},
                                         builder_.getPtrTy(), parameter.name + ".base");
            llvm::Value* stride = captured({Capture::Kind::Parameter, parameter.name, field, true, 3},
                                           builder_.getInt64Ty(), parameter.name + ".stride");
            flatStreams_.emplace(field, StreamRead{flat, value, base, stride});
            result = base;
        }
        entryLoads_.emplace(std::make_pair(field, stream), result);
        return result;
    }

    // A capture's value in the function around the region, where the region starts: the variable's (loaded), its
    // address, or what the method read of the parameter. In a region itself, these capture in turn.
    llvm::Value* Lowering::outerValue(const Capture& capture)
    {
        switch (capture.kind)
        {
        case Capture::Kind::Value:
        {
            const Binding* binding = find(capture.name);
            llvm::LoadInst* load = builder_.CreateLoad(typeOf(binding->type), binding->slot, capture.name);
            tag(load, binding->type);
            return load;
        }
        case Capture::Kind::Reference:
            return find(capture.name)->slot;
        case Capture::Kind::Parameter:
            if (!capture.stream)
                return atEntry(capture.field, false);
            atEntry(capture.field, true);
            const StreamRead& read = flatStreams_.at(capture.field);
            const std::array<llvm::Value*, 4> parts{read.flat, read.value, read.base, read.stride};
            return parts.at(capture.part);
        }
        return nullptr;
    }
} // namespace fminus::ir::detail
