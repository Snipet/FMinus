// python/src/Module.cpp: the extension module fminus._fminus, FMinus's public API (include/fminus/FMinus.h) as Python
// objects. It is a host like any other: C++17, FMinus::FMinus alone, nothing of FMinus's internals. python/fminus/
// is the Python around it (compile() and load(), with their keyword arguments and the files #include reads), and
// docs/python.md the manual.
//
//   Program    a compiled program (fminus::Program, shared by everything made from it, so it lives while any does)
//   Function   one of its functions, called by name through Program::call(), which checks every argument against
//              the declaration: a wrong call is a TypeError, never undefined behaviour. A method's may be bound to
//              the Instance it runs on.
//   Module     a module's description (Program::Module): its size, fields, methods and parameters
//   Instance   a module's instance: memory this object owns, zeroed and aligned as Program::Module says, its
//              parameters at their defaults
//
// What crosses, as Program::call() passes it: bool, int, float and double by value, converted from Python's numbers to
// the parameter's type, and pointers to memory Python lends for the call, through its buffer protocol (array.array, a
// NumPy array, a memoryview, a ctypes array): a `float*` takes a buffer of C floats, `int*` of C ints, `double*` of C
// doubles, `void*` any, and a module's pointer an Instance. None is a null pointer. A pointer has no extent in C, and
// FMinus checks no access through one: a function that is passed a buffer is passed its length too, and a length longer
// than the buffer reads or writes past it, as it would from C++.
//
// The GIL is released while the function runs, so other Python threads run meanwhile, and a program's parallel regions
// have the machine's cores. Compiling holds it (the include resolver is Python's).
#include "fminus/FMinus.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace py = pybind11;
using namespace py::literals;

namespace
{
    using fminus::Program;
    using Kind = Program::Value::Kind;

    // fminus.Error and the errors under it (made in the module's initialiser, which owns them).
    PyObject* errorType = nullptr;             // fminus.Error
    PyObject* compileErrorType = nullptr;      // fminus.CompileError
    PyObject* loopBudgetErrorType = nullptr;   // fminus.LoopBudgetError
    PyObject* boundsErrorType = nullptr;       // fminus.BoundsError

    [[noreturn]] void raise(PyObject* type, const std::string& message)
    {
        PyErr_SetString(type, message.c_str());
        throw py::error_already_set();
    }

    // FMinus's text as a str. It is UTF-8 as the source had it, except a string literal's escapes ("\xff"), which may
    // be any bytes: those read as U+FFFD.
    py::str text(std::string_view bytes)
    {
        return py::reinterpret_steal<py::str>(
            PyUnicode_DecodeUTF8(bytes.data(), static_cast<Py_ssize_t>(bytes.size()), "replace"));
    }

    std::string quoted(std::string_view name)
    {
        return "'" + std::string(name) + "'";
    }

    // One line, as an exception's message is: FMinus ends its messages with a newline.
    std::string trimmed(std::string message)
    {
        while (!message.empty() && message.back() == '\n')
            message.pop_back();
        return message;
    }

    // A record of named values, read as attributes and printed whole: types.SimpleNamespace.
    py::object record(const py::dict& fields)
    {
        return py::module_::import("types").attr("SimpleNamespace")(**fields);
    }

    // ---- options ----

    template <typename Enum>
    Enum named(const char* option, const std::string& value,
               std::initializer_list<std::pair<std::string_view, Enum>> names)
    {
        std::string choices;
        for (const auto& [name, choice] : names)
        {
            if (value == name)
                return choice;
            choices += (choices.empty() ? "" : ", ") + quoted(name);
        }
        throw py::value_error(std::string(option) + " is " + quoted(value) + "; it is one of " + choices);
    }

    // The include resolver's Python side. An exception the callable raises cannot cross FMinus's compiler, so it is
    // kept here, the #include finds no file, and compile() raises it when the compiler has returned.
    struct Includes
    {
        py::object resolve;
        std::optional<py::error_already_set> raised;
    };

    std::shared_ptr<Program> compile(const std::string& source, const std::string& fileName, int optimisation,
                                     unsigned polyLanes, const std::string& math, const py::object& include,
                                     const std::map<std::string, std::string>& defines, const py::dict& parameters,
                                     bool strictAliasing, unsigned knobVersions, const std::string& denormals,
                                     unsigned threads, const std::string& keepIr, bool loopBudget, bool boundsCheck)
    {
        Program::Options options;
        options.fileName = fileName;
        options.optimisation = optimisation;
        options.polyLanes = polyLanes;
        options.math = named<Program::Math>(
            "math", math,
            {{"vector", Program::Math::Vector}, {"libm", Program::Math::Libm}, {"fast", Program::Math::Fast}});
        options.defines = defines;
        for (const auto& [name, binding] : parameters)
        {
            // "knob", "stream", or a number: the value it is frozen at
            if (py::isinstance<py::str>(binding))
            {
                options.parameters[name.cast<std::string>()] = named<Program::Binding>(
                    "a parameter's binding", binding.cast<std::string>(),
                    {{"knob", Program::Binding::knob()}, {"stream", Program::Binding::stream()}});
            }
            else
                options.parameters[name.cast<std::string>()] = Program::Binding::frozen(binding.cast<double>());
        }
        options.strictAliasing = strictAliasing;
        options.knobVersions = knobVersions;
        options.denormals = named<Program::Denormals>(
            "denormals", denormals, {{"preserve", Program::Denormals::Preserve}, {"flush", Program::Denormals::Flush}});
        options.threads = threads;
        options.keepIr = named<Program::KeepIr>("keep_ir", keepIr,
                                                {{"none", Program::KeepIr::None},
                                                 {"lowered", Program::KeepIr::Lowered},
                                                 {"optimised", Program::KeepIr::Optimised},
                                                 {"both", Program::KeepIr::Both}});
        options.loopBudget = loopBudget;
        options.boundsCheck = boundsCheck;

        const auto includes = std::make_shared<Includes>();
        if (!include.is_none())
        {
            includes->resolve = include;
            options.include = [includes](std::string_view name,
                                         std::string_view from) -> std::optional<Program::IncludedFile>
            {
                if (includes->raised)
                    return std::nullopt;
                try
                {
                    const py::object found = includes->resolve(text(name), text(from));
                    if (found.is_none())
                        return std::nullopt;
                    auto [foundName, foundSource] = found.cast<std::pair<std::string, std::string>>();
                    return Program::IncludedFile{std::move(foundName), std::move(foundSource)};
                }
                catch (py::error_already_set& error)
                {
                    includes->raised.emplace(std::move(error));
                }
                catch (const py::cast_error&)
                {
                    PyErr_SetString(PyExc_TypeError, "an include resolver returns (file_name, source) or None");
                    includes->raised.emplace();
                }
                return std::nullopt;
            };
        }

        std::string errors;
        std::unique_ptr<Program> program = Program::compile(source, errors, options);
        // The resolver is the compile's only: a Program that kept it would hold Python objects.
        includes->resolve = py::object();
        if (includes->raised)
        {
            py::error_already_set error = std::move(*includes->raised);
            includes->raised.reset();
            throw error;
        }
        if (!program)
            raise(compileErrorType, trimmed(std::move(errors)));
        return program;
    }

    // ---- descriptions ----

    py::list attributesOf(const std::vector<Program::Attribute>& attributes)
    {
        py::list list;
        for (const Program::Attribute& attribute : attributes)
        {
            py::list arguments;
            for (const Program::Attribute::Argument& argument : attribute.arguments)
            {
                if (argument.isString)
                    arguments.append(text(argument.text));
                else
                    arguments.append(argument.number);
            }
            list.append(record(py::dict("name"_a = text(attribute.name), "arguments"_a = arguments)));
        }
        return list;
    }

    const char* bindingName(Program::Binding::Kind kind)
    {
        switch (kind)
        {
        case Program::Binding::Kind::Knob:
            return "knob";
        case Program::Binding::Kind::Stream:
            return "stream";
        case Program::Binding::Kind::Frozen:
            return "frozen";
        }
        return "knob";
    }

    py::object parameterOf(const Program::Parameter& parameter)
    {
        const bool frozen = parameter.binding.kind == Program::Binding::Kind::Frozen;
        return record(py::dict("module"_a = text(parameter.module), "name"_a = text(parameter.name),
                               "type"_a = text(parameter.type), "binding"_a = bindingName(parameter.binding.kind),
                               "frozen"_a = frozen ? py::object(py::float_(parameter.binding.value)) : py::none(),
                               "minimum"_a = parameter.minimum, "maximum"_a = parameter.maximum,
                               "default"_a = parameter.defaultValue, "stream_offset"_a = parameter.streamOffset,
                               "value_offset"_a = parameter.valueOffset,
                               "attributes"_a = attributesOf(parameter.attributes)));
    }

    py::object fieldOf(const Program::Module::Field& field)
    {
        return record(py::dict(
            "name"_a = text(field.name), "type"_a = text(field.type), "offset"_a = field.offset, "size"_a = field.size,
            "element"_a = field.element.empty() ? py::object(py::none()) : py::object(text(field.element)),
            "extents"_a = py::tuple(py::cast(field.extents)), "attributes"_a = attributesOf(field.attributes)));
    }

    py::object violationOf(const Program::BoundsViolation& violation)
    {
        return record(py::dict(
            "file"_a = text(violation.file), "line"_a = violation.line, "column"_a = violation.column,
            "array"_a = text(violation.array), "type"_a = text(violation.type), "extent"_a = violation.extent,
            "index"_a = violation.index, "width"_a = violation.width, "message"_a = text(violation.message())));
    }

    const char* codeKindName(Program::CodeRange::Kind kind)
    {
        using Code = Program::CodeRange::Kind;
        switch (kind)
        {
        case Code::Function:
            return "function";
        case Code::EntryPoint:
            return "entry_point";
        case Code::Parallel:
            return "parallel";
        case Code::Thunk:
            return "thunk";
        case Code::Support:
            return "support";
        }
        return "support";
    }

    // ---- modules and their instances ----

    struct Module
    {
        std::shared_ptr<Program> program;
        const Program::Module* module = nullptr;   // the Program's
    };

    // A module's instance: its memory, which this object owns, and the buffers its stream parameters point into, which
    // it holds for as long as they do.
    struct Instance
    {
        std::shared_ptr<Program> program;
        const Program::Module* module = nullptr;
        void* memory = nullptr;
        std::size_t alignment = alignof(std::max_align_t);
        std::size_t allocated = 0;
        std::map<std::string, Py_buffer> streams;

        Instance(std::shared_ptr<Program> itsProgram, const Program::Module& itsModule)
            : program(std::move(itsProgram))
            , module(&itsModule)
        {
            alignment = std::max(module->alignment, alignof(std::max_align_t));
            allocated = (std::max<std::size_t>(module->size, 1) + alignment - 1) / alignment * alignment;
            memory = ::operator new(allocated, std::align_val_t(alignment));
            reset();
        }

        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        ~Instance()
        {
            releaseStreams();
            ::operator delete(memory, std::align_val_t(alignment));
        }

        // As it was made: every field 0, every parameter at its default, no stream.
        void reset()
        {
            releaseStreams();
            std::memset(memory, 0, allocated);
            Program::setDefaults(memory, *module);
        }

        void releaseStreams()
        {
            for (auto& [name, view] : streams)
                PyBuffer_Release(&view);
            streams.clear();
        }

        const Program::Parameter& parameter(const std::string& name) const
        {
            for (const Program::Parameter& candidate : module->parameters)
            {
                if (candidate.name == name)
                    return candidate;
            }
            throw py::key_error("module " + quoted(module->name) + " has no parameter " + quoted(name));
        }
    };

    // ---- functions ----

    struct Function
    {
        std::shared_ptr<Program> program;
        const Program::Callable* callable = nullptr;   // the Program's
        py::object self;                               // a bound method's Instance; none otherwise
    };

    Function lookup(const std::shared_ptr<Program>& program, const std::string& name, py::object self, bool attribute)
    {
        std::string errors;
        const Program::Callable* callable = program->callable(name, errors);
        if (callable == nullptr)
        {
            if (attribute)
                throw py::attribute_error(trimmed(std::move(errors)));
            raise(errorType, trimmed(std::move(errors)));
        }
        return Function{program, callable, std::move(self)};
    }

    std::string signatureOf(const Function& function)
    {
        const Program::Callable& callable = *function.callable;
        std::string signature = callable.resultType + " " + callable.name + "(";
        for (std::size_t i = function.self ? 1 : 0; i < callable.parameters.size(); ++i)
        {
            const Program::Callable::Param& param = callable.parameters[i];
            if (signature.back() != '(')
                signature += ", ";
            signature += param.type;
            if (!param.name.empty())
                signature += (param.type.back() == '*' ? "" : " ") + param.name;
        }
        return signature + ")";
    }

    // "argument 2 of 'softclip' <what> (its parameter 'drive')", as FMinus words a call's mismatch. A bound method's
    // arguments count from the first after its instance.
    std::string wrongArgument(const Function& function, std::size_t index, const std::string& what)
    {
        const Program::Callable::Param& param = function.callable->parameters[index];
        std::string message = "argument " + std::to_string(index + (function.self ? 0 : 1)) + " of " +
                              quoted(function.callable->name) + " " + what;
        if (!param.name.empty())
            message += " (its parameter " + quoted(param.name) + ")";
        return message;
    }

    std::string typeName(py::handle object)
    {
        return quoted(Py_TYPE(object.ptr())->tp_name);
    }

    Program::Value scalarArgument(const Function& function, std::size_t index, py::handle argument)
    {
        const Program::Callable::Param& param = function.callable->parameters[index];
        const auto wrong = [&](const char* wanted)
        {
            return py::type_error(
                wrongArgument(function, index,
                              "is a " + typeName(argument) + ", where a " + quoted(param.type) + " takes " + wanted));
        };
        switch (param.kind)
        {
        case Kind::Bool:
        {
            // a bool, or an int's 0 or not; NumPy's bool_ is neither to Python, and is one here
            const std::string_view type = Py_TYPE(argument.ptr())->tp_name;
            if (!PyBool_Check(argument.ptr()) && !PyIndex_Check(argument.ptr()) && type != "numpy.bool" &&
                type != "numpy.bool_")
                throw wrong("a bool");
            const int truth = PyObject_IsTrue(argument.ptr());
            if (truth < 0)
                throw py::error_already_set();
            return Program::Value::of(truth != 0);
        }
        case Kind::Int:
        {
            if (!PyIndex_Check(argument.ptr()))   // an int, or what stands for one (a NumPy integer): no float
                throw wrong("an int");
            const py::object index_ = py::reinterpret_steal<py::object>(PyNumber_Index(argument.ptr()));
            if (!index_)
                throw py::error_already_set();
            int overflow = 0;
            const long long value = PyLong_AsLongLongAndOverflow(index_.ptr(), &overflow);
            if (value == -1 && PyErr_Occurred() != nullptr)
                throw py::error_already_set();
            if (overflow != 0 || value < std::numeric_limits<std::int32_t>::min() ||
                value > std::numeric_limits<std::int32_t>::max())
                raise(PyExc_OverflowError, wrongArgument(function, index, "does not fit an 'int' (32 bits)"));
            return Program::Value::of(static_cast<int>(value));
        }
        case Kind::Float:
        case Kind::Double:
        {
            if (PyUnicode_Check(argument.ptr()) || PyBytes_Check(argument.ptr()))
                throw wrong("a number");
            const double value = PyFloat_AsDouble(argument.ptr());   // a float, an int, or what has __float__
            if (value == -1.0 && PyErr_Occurred() != nullptr)
            {
                if (PyErr_ExceptionMatches(PyExc_TypeError) == 0)
                    throw py::error_already_set();
                PyErr_Clear();
                throw wrong("a number");
            }
            return Program::Value::scalar(param.kind, value);   // a float's rounded to one
        }
        case Kind::Void:
        case Kind::Pointer:
            break;
        }
        throw wrong("a value");
    }

    // What a buffer's elements are, from its struct format: C's float, double or int in this machine's byte order, or
    // anything else (Void), which only a void* takes.
    Kind elementOf(const Py_buffer& view)
    {
        std::string_view format = view.format != nullptr ? view.format : "B";
        const std::uint16_t one = 1;
        const bool little = *reinterpret_cast<const unsigned char*>(&one) == 1;
        // '@' and '=' are this machine's order, and so is '<' or '>' when it names it (ctypes' arrays do)
        if (!format.empty() &&
            (format.front() == '@' || format.front() == '=' || format.front() == (little ? '<' : '>')))
            format.remove_prefix(1);
        if (format == "f" && view.itemsize == sizeof(float))
            return Kind::Float;
        if (format == "d" && view.itemsize == sizeof(double))
            return Kind::Double;
        if ((format == "i" || format == "l") && view.itemsize == sizeof(int))
            return Kind::Int;
        return Kind::Void;
    }

    // The buffers lent for a call, given back when it has returned.
    struct Lent
    {
        std::vector<Py_buffer> views;

        Lent() = default;
        Lent(const Lent&) = delete;
        Lent& operator=(const Lent&) = delete;

        ~Lent()
        {
            for (Py_buffer& view : views)
                PyBuffer_Release(&view);
        }
    };

    Program::Value pointerArgument(const Function& function, std::size_t index, py::handle argument, Lent& lent)
    {
        const Program::Callable::Param& param = function.callable->parameters[index];
        Program::Value value = Program::Value::array(static_cast<float*>(nullptr));
        if (argument.is_none())   // a null pointer, of the parameter's type
        {
            value.element = param.element;
            value.module = param.module;
            value.readOnly = param.readOnly;
            return value;
        }
        if (py::isinstance<Instance>(argument))
        {
            const Instance& instance = argument.cast<const Instance&>();
            return Program::Value::instance(instance.memory, *instance.module);
        }
        if (PyObject_CheckBuffer(argument.ptr()) == 0)
        {
            throw py::type_error(wrongArgument(function, index,
                                               "is a " + typeName(argument) +
                                                   ", not a buffer (an array.array, a "
                                                   "NumPy array, a memoryview), an Instance or None for a " +
                                                   quoted(param.type)));
        }
        Py_buffer view;
        if (PyObject_GetBuffer(argument.ptr(), &view, PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) != 0)
        {
            PyErr_Clear();
            throw py::type_error(wrongArgument(function, index, "is not one block of memory (a C-contiguous buffer)"));
        }
        lent.views.push_back(view);
        if (view.readonly != 0 && !param.readOnly)
        {
            throw py::type_error(wrongArgument(function, index,
                                               "is a read-only " + typeName(argument) + ", and a " +
                                                   quoted(param.type) + " may be written through"));
        }
        const Kind element = elementOf(view);
        const bool anything = param.element == Kind::Void && param.module == nullptr;   // void*
        if (element == Kind::Void && !anything)
        {
            throw py::type_error(wrongArgument(function, index,
                                               "is a buffer of format " +
                                                   quoted(view.format != nullptr ? view.format : "B") + ", not a " +
                                                   quoted(param.type)));
        }
        value.pointer = view.buf;
        value.element = element == Kind::Void ? Kind::Float : element;   // a void* takes any; call() checks the rest
        value.readOnly = view.readonly != 0;
        return value;
    }

    py::object call(const Function& function, const py::args& arguments)
    {
        const Program::Callable& callable = *function.callable;
        const std::size_t bound = function.self ? 1 : 0;
        const std::size_t count = arguments.size() + bound;
        if (count != callable.parameters.size())
        {
            const std::size_t takes = callable.parameters.size() - bound;
            throw py::type_error(quoted(callable.name) + " takes " + std::to_string(takes) + " argument" +
                                 (takes == 1 ? "" : "s") + ", not " + std::to_string(arguments.size()));
        }
        constexpr std::size_t kOnStack = 16;
        std::array<Program::Value, kOnStack> local;
        std::vector<Program::Value> many;
        Program::Value* values = local.data();
        if (count > kOnStack)
        {
            many.resize(count);
            values = many.data();
        }
        Lent lent;
        lent.views.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            const py::handle argument = i < bound ? py::handle(function.self) : py::handle(arguments[i - bound]);
            values[i] = callable.parameters[i].kind == Kind::Pointer ? pointerArgument(function, i, argument, lent)
                                                                     : scalarArgument(function, i, argument);
        }
        Program::CallResult result;
        {
            const py::gil_scoped_release release;
            result = function.program->call(callable, values, count);
        }
        using Status = Program::CallResult::Status;
        switch (result.status)
        {
        case Status::Ok:
            break;
        case Status::Mismatch:
            throw py::type_error(result.message());
        case Status::OutOfBudget:
            raise(loopBudgetErrorType, result.message());
        case Status::OutOfBounds:
            raise(boundsErrorType, result.message());
        case Status::NoFunction:
        case Status::Unsupported:
        case Status::LinkError:
            raise(errorType, result.message());
        }
        switch (result.value.kind)
        {
        case Kind::Bool:
            return py::bool_(result.value.number != 0.0);
        case Kind::Int:
            return py::int_(static_cast<long long>(result.value.number));
        case Kind::Float:
        case Kind::Double:
            return py::float_(result.value.number);
        case Kind::Void:
        case Kind::Pointer:
            break;
        }
        return py::none();
    }

    template <typename Class>
    void inPackage(Class& type)
    {
        type.attr("__module__") = "fminus";   // as its users name it, not fminus._fminus
    }

    PyObject* newError(py::module_& module, const char* name, PyObject* base, const char* doc)
    {
        const std::string qualified = std::string("fminus.") + name;
        PyObject* type = PyErr_NewExceptionWithDoc(qualified.c_str(), doc, base, nullptr);
        if (type == nullptr)
            throw py::error_already_set();
        module.add_object(name, py::reinterpret_borrow<py::object>(type));
        return type;   // the reference made here is kept for the module's life
    }
}   // namespace

PYBIND11_MODULE(_fminus, m)
{
    m.doc() = "FMinus's extension module: fminus.Program and what it gives. Import fminus instead.";
    m.attr("__version__") = FMINUS_VERSION_STRING;
    m.attr("llvm_version") = FMINUS_LLVM_PACKAGE_VERSION;

    errorType = newError(m, "Error", PyExc_Exception, "An error of FMinus's: the base of the others.");
    compileErrorType = newError(m, "CompileError", errorType,
                                "The source does not compile. Its message is FMinus's errors, one a line, as clang "
                                "words them.");
    loopBudgetErrorType =
        newError(m, "LoopBudgetError", errorType, "A call ran past the program's loop budget and returned early.");
    boundsErrorType =
        newError(m, "BoundsError", errorType, "A bounds check failed, and the call returned early, before the access.");

    m.def("_compile", &compile, "source"_a, "file_name"_a, "optimisation"_a, "poly_lanes"_a, "math"_a, "include"_a,
          "defines"_a, "parameters"_a, "strict_aliasing"_a, "knob_versions"_a, "denormals"_a, "threads"_a, "keep_ir"_a,
          "loop_budget"_a, "bounds_check"_a, "Compiles source (fminus.compile's arguments, in order).");

    // Every class first, then their methods: a method's docstring then names the others as Python does.
    py::class_<Program, std::shared_ptr<Program>> program(
        m, "Program", "A compiled FMinus program: fminus.compile() or fminus.load() makes one.");
    inPackage(program);
    py::class_<Function> function(m, "Function",
                                  "A function of a Program. Call it: each argument is checked against the "
                                  "declaration, and a wrong one is a TypeError.");
    inPackage(function);
    py::class_<Module> module(m, "Module", "A module of a Program (FMinus's class): its layout, and its instances.");
    inPackage(module);
    py::class_<Instance> instance(m, "Instance", py::buffer_protocol(),
                                  "An instance of a module: its memory, which this object owns. Its methods are "
                                  "attributes (instance.tick(x)), its parameters items (instance['cutoff'] = 1000), "
                                  "and its bytes a buffer (memoryview(instance)), laid out as Module.fields says.");
    inPackage(instance);

    program
        .def(
            "function", [](const std::shared_ptr<Program>& self, const std::string& name)
            { return lookup(self, name, py::object(), false); }, "name"_a,
            "The function of the name, a method's as 'Osc::tick' (its first argument the Instance).")
        .def(
            "__getattr__", [](const std::shared_ptr<Program>& self, const std::string& name)
            { return lookup(self, name, py::object(), true); }, "name"_a)
        .def(
            "module",
            [](const std::shared_ptr<Program>& self, const std::string& name)
            {
                const Program::Module* module = self->module(name);
                if (module == nullptr)
                    raise(errorType, "the program has no module " + quoted(name));
                return Module{self, module};
            },
            "name"_a, "The module of the name.")
        .def_property_readonly(
            "modules",
            [](const std::shared_ptr<Program>& self)
            {
                py::list modules;
                for (const Program::Module& module : self->modules())
                    modules.append(Module{self, &module});
                return modules;
            },
            "Every module of the program, in declaration order.")
        .def(
            "address",
            [](Program& self, const std::string& name)
            {
                std::string errors;
                const std::uintptr_t address = self.address(name, errors);
                if (address == 0)
                    raise(errorType, trimmed(std::move(errors)));
                return address;
            },
            "name"_a,
            "The function's address, an int, for ctypes or cffi: a C function of its declaration's type, valid "
            "while the Program lives. Nothing checks a call through it.")
        .def_property_readonly(
            "warnings", [](const Program& self) { return text(self.warnings()); },
            "The program's warnings, one a line as clang prints them; empty when it has none.")
        .def_property_readonly("poly_lanes", &Program::polyLanes, "How many lanes a poly float has in this program.")
        .def_property_readonly(
            "lowered_ir", [](const Program& self) { return text(self.loweredIr()); },
            "The LLVM IR as the code generator wrote it, when keep_ir kept it ('lowered' or 'both'); else empty.")
        .def_property_readonly(
            "optimised_ir", [](const Program& self) { return text(self.optimisedIr()); },
            "The LLVM IR the machine code is generated from, when keep_ir kept it ('optimised' or 'both'); else "
            "empty.")
        .def(
            "nonblocking", [](const Program& self, const std::string& name) { return self.nonBlocking(name); },
            "name"_a, "Whether the function is marked [[clang::nonblocking]].")
        .def(
            "nonallocating", [](const Program& self, const std::string& name) { return self.nonAllocating(name); },
            "name"_a, "Whether the function is marked [[clang::nonallocating]] or [[clang::nonblocking]].")
        .def(
            "set_loop_budget", [](Program& self, std::int64_t backEdges) { self.setLoopBudget(backEdges); },
            "back_edges"_a,
            "Sets the loop budget for the next call (a program compiled with loop_budget=True), and clears the flag.")
        .def_property_readonly("loop_budget_left", &Program::loopBudgetLeft,
                               "What is left of the loop budget; -1 once a call has run out.")
        .def_property_readonly("loop_budget_exceeded", &Program::loopBudgetExceeded,
                               "Whether a call since set_loop_budget() ran out of the budget.")
        .def_property_readonly(
            "bounds_violation",
            [](const Program& self) -> py::object
            {
                const std::optional<Program::BoundsViolation> violation = self.boundsViolation();
                return violation ? violationOf(*violation) : py::object(py::none());
            },
            "The first bounds check that failed since clear_bounds_violation() (a program compiled with "
            "bounds_check=True), or None.")
        .def("clear_bounds_violation", &Program::clearBoundsViolation,
             "Clears the record of a failed bounds check, so that the next call runs whole.")
        .def(
            "code_ranges",
            [](Program& self)
            {
                std::string errors;
                const std::vector<Program::CodeRange> ranges = self.codeRanges(errors);
                if (!errors.empty())
                    raise(errorType, trimmed(std::move(errors)));
                py::list list;
                for (const Program::CodeRange& range : ranges)
                {
                    list.append(record(py::dict("address"_a = range.address, "size"_a = range.size,
                                                "kind"_a = codeKindName(range.kind), "symbol"_a = text(range.symbol),
                                                "function"_a = text(range.function), "module"_a = text(range.module),
                                                "inlined"_a = range.inlined)));
                }
                return list;
            },
            "Where the program's machine code is, a record a range, sorted by address (for a profiler).");

    function.def("__call__", &call)
        .def_property_readonly(
            "name", [](const Function& self) { return text(self.callable->name); },
            "Its name, a method's as 'Osc::tick'.")
        .def_property_readonly(
            "signature", [](const Function& self) { return text(signatureOf(self)); },
            "Its declaration, as FMinus writes the types: 'float softclip(float x, float drive)'.")
        .def_property_readonly(
            "result_type", [](const Function& self) { return text(self.callable->resultType); },
            "Its result's type, as FMinus writes it: 'float', 'void'.")
        .def_property_readonly(
            "parameters",
            [](const Function& self)
            {
                py::list parameters;
                for (std::size_t i = self.self ? 1 : 0; i < self.callable->parameters.size(); ++i)
                {
                    const Program::Callable::Param& param = self.callable->parameters[i];
                    parameters.append(record(py::dict("name"_a = text(param.name), "type"_a = text(param.type))));
                }
                return parameters;
            },
            "Its parameters, each a record of a name and a type ('const float *'); a bound method's without its "
            "instance.")
        .def_property_readonly(
            "module",
            [](const Function& self) -> py::object
            {
                if (self.callable->module == nullptr)
                    return py::none();
                return text(self.callable->module->name);
            },
            "A method's module's name; None for a function that is no method.")
        .def_property_readonly(
            "instance", [](const Function& self) { return self.self ? self.self : py::object(py::none()); },
            "The Instance a bound method runs on; None otherwise.")
        .def_property_readonly(
            "nonblocking", [](const Function& self) { return self.program->nonBlocking(self.callable->name); },
            "Whether it is marked [[clang::nonblocking]].")
        .def_property_readonly(
            "nonallocating", [](const Function& self) { return self.program->nonAllocating(self.callable->name); },
            "Whether it is marked [[clang::nonallocating]] or [[clang::nonblocking]].")
        .def("__repr__", [](const Function& self) { return "<fminus.Function " + signatureOf(self) + ">"; });

    module.def_property_readonly("name", [](const Module& self) { return text(self.module->name); })
        .def_property_readonly(
            "size", [](const Module& self) { return self.module->size; }, "An instance's size, in bytes.")
        .def_property_readonly(
            "alignment", [](const Module& self) { return self.module->alignment; }, "An instance's alignment.")
        .def_property_readonly(
            "fields",
            [](const Module& self)
            {
                py::list fields;
                for (const Program::Module::Field& field : self.module->fields)
                    fields.append(fieldOf(field));
                return fields;
            },
            "Its fields in declaration order, each a record: name, type, offset and size in bytes, element and "
            "extents (an array's), attributes.")
        .def_property_readonly(
            "methods",
            [](const Module& self)
            {
                py::list methods;
                for (const std::string& method : self.module->methods)
                    methods.append(text(method));
                return methods;
            },
            "Its methods' names, as Program.function() takes them: 'Osc::tick'.")
        .def_property_readonly(
            "parameters",
            [](const Module& self)
            {
                py::list parameters;
                for (const Program::Parameter& parameter : self.module->parameters)
                    parameters.append(parameterOf(parameter));
                return parameters;
            },
            "Its parameters in declaration order, each a record: name, type, binding ('knob', 'stream' or "
            "'frozen'), minimum, maximum, default, attributes.")
        .def(
            "instance", [](const Module& self) { return std::make_unique<Instance>(self.program, *self.module); },
            "A new instance: zeroed, its parameters at their defaults.")
        .def(
            "__call__", [](const Module& self) { return std::make_unique<Instance>(self.program, *self.module); },
            "A new instance, as instance().")
        .def("__repr__", [](const Module& self) { return "<fminus.Module " + self.module->name + ">"; });

    instance
        .def_buffer(
            [](Instance& self)
            {
                return py::buffer_info(self.memory, 1, "B", 1, {static_cast<py::ssize_t>(self.module->size)},
                                       {static_cast<py::ssize_t>(1)});
            })
        .def_property_readonly(
            "module", [](const Instance& self) { return Module{self.program, self.module}; }, "Its Module.")
        .def(
            "__getattr__",
            [](const py::object& self, const std::string& name)
            {
                const Instance& its = self.cast<const Instance&>();
                return lookup(its.program, its.module->name + "::" + name, self, true);
            },
            "name"_a)
        .def(
            "get", [](const Instance& self, const std::string& name)
            { return Program::value(self.memory, self.parameter(name)); }, "name"_a,
            "A parameter's value, as set() last set it.")
        .def(
            "set", [](Instance& self, const std::string& name, double value)
            { Program::setValue(self.memory, self.parameter(name), value); }, "name"_a, "value"_a,
            "Sets a parameter's value (a knob's, or a stream's for a call without samples), from any thread.")
        .def("__getitem__", [](const Instance& self, const std::string& name)
             { return Program::value(self.memory, self.parameter(name)); })
        .def("__setitem__", [](Instance& self, const std::string& name, double value)
             { Program::setValue(self.memory, self.parameter(name), value); })
        .def(
            "set_stream",
            [](Instance& self, const std::string& name, const py::object& samples)
            {
                const Program::Parameter& parameter = self.parameter(name);
                const std::string qualified = parameter.module + "::" + parameter.name;
                if (parameter.binding.kind != Program::Binding::Kind::Stream)
                    raise(errorType, quoted(qualified) + " is no stream in this program: compile it with parameters={" +
                                         quoted(qualified) + ": 'stream'}");
                Py_buffer view;
                const float* pointer = nullptr;
                if (!samples.is_none())
                {
                    if (PyObject_GetBuffer(samples.ptr(), &view, PyBUF_FORMAT | PyBUF_C_CONTIGUOUS) != 0)
                    {
                        PyErr_Clear();
                        throw py::type_error("a stream's samples are a buffer of floats (array.array('f'), a NumPy "
                                             "float32 array) or None");
                    }
                    if (elementOf(view) != Kind::Float)
                    {
                        PyBuffer_Release(&view);
                        throw py::type_error("a stream's samples are a buffer of floats (array.array('f'), a NumPy "
                                             "float32 array) or None");
                    }
                    pointer = static_cast<const float*>(view.buf);
                }
                if (const auto held = self.streams.find(name); held != self.streams.end())
                {
                    PyBuffer_Release(&held->second);
                    self.streams.erase(held);
                }
                if (pointer != nullptr)
                    self.streams.emplace(name, view);
                else if (!samples.is_none())
                    PyBuffer_Release(&view);   // no samples: a flat call
                Program::setStream(self.memory, parameter, pointer);
            },
            "name"_a, "samples"_a,
            "Points a stream parameter at its samples, a buffer of floats as long as the next calls read, which the "
            "instance holds until another is set; None makes the calls flat, every sample the parameter's value.")
        .def(
            "set_defaults", [](Instance& self) { Program::setDefaults(self.memory, *self.module); },
            "Sets each parameter that has an fminus::default to it.")
        .def("reset", &Instance::reset, "As it was made: every field 0, every parameter at its default, no stream.")
        .def("__repr__", [](const Instance& self) { return "<fminus.Instance of " + self.module->name + ">"; });
}
