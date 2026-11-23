// host.cpp: the experiment's host. A funkgui::Panel whose every method forwards to a compiled FMinus program
// (counter.fm), the C functions that program calls to draw (ui.fm's prototypes, over funkgui::Canvas), and a scripted
// run of it on funkgui::HeadlessHost: no window, no GPU, no JUCE. Built twice from this one file:
//
//   counter-jit (APP_JIT)   compiles counter.fm while it runs (fminus::Program), then compiles it again as its edit
//                           (-D V2) and carries the running state over: a reload.
//   counter-aot             links counter.o, which `fminus -c counter.fm` wrote ahead of time; no compiler inside.
//
// Usage: counter-jit <dir of counter.fm> <output dir>      counter-aot <output dir>
#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/text/FontService.h>

#if APP_JIT
  #include "fminus/FMinus.h"
#endif

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ---- what the FMinus program calls ----------------------------------------------------------------------------------
// One frame at a time: the Panel points these at the Canvas it was handed, for the length of app_draw.

namespace
{
    funkgui::Canvas* gCanvas = nullptr;
    const funkgui::Theme* gTheme = nullptr;
    std::vector<funkgui::A11yItem>* gItems = nullptr;

    funkgui::Col ink(int token, float alpha)
    {
        const funkgui::Theme& t = *gTheme;
        const funkgui::Col tokens[] = { t.ground, t.ink100, t.ink70, t.ink52, t.ink32, t.ink16, t.accent, t.accentDim,
                                        t.signal };
        return funkgui::fade(tokens[token >= 0 && token < 9 ? token : 0], alpha);
    }

    const funkgui::TextStyle& style(int index)
    {
        namespace T = funkgui::type;
        static const funkgui::TextStyle styles[] = { T::kDisplay, T::kValueP, T::kValueS, T::kNumeral, T::kWordmark,
                                                     T::kLatch, T::kLabel, T::kCaption, T::kMicro, T::kUnit };
        return styles[index >= 0 && index < 10 ? index : 6];
    }

    funkgui::Align align(int a) { return a == 1 ? funkgui::Align::centre : a == 2 ? funkgui::Align::right : funkgui::Align::left; }
}

#define UI_EXPORT extern "C" __attribute__((visibility("default")))   // the JIT finds them in the process by name

UI_EXPORT void ui_rrect(float x, float y, float w, float h, float radius, int token, float alpha)
{
    if (gCanvas != nullptr && alpha > 0.0f)
        gCanvas->rrect(x, y, w, h, radius, ink(token, alpha));
}

UI_EXPORT void ui_frame(float x, float y, float w, float h, float radius, float border, int token, float alpha)
{
    if (gCanvas != nullptr)
        gCanvas->rrect(x, y, w, h, radius, funkgui::Col{ 0, 0, 0, 0 }, border, ink(token, alpha));
}

UI_EXPORT void ui_hairline(float x, float y, float w, int token, float alpha)
{
    if (gCanvas != nullptr)
        gCanvas->hairlineH(x, y, w, ink(token, alpha));
}

UI_EXPORT void ui_disc(float cx, float cy, float r, int token, float alpha)
{
    if (gCanvas != nullptr)
        gCanvas->disc(cx, cy, r, ink(token, alpha));
}

UI_EXPORT void ui_text_bytes(const void* utf8, int bytes, float x, float y, int styleIndex, int token, int alignment)
{
    if (gCanvas == nullptr || utf8 == nullptr || bytes <= 0)
        return;
    char text[256];                                   // Canvas::text wants a NUL; a string has none
    const size_t n = std::min<size_t>(static_cast<size_t>(bytes), sizeof text - 1);
    std::memcpy(text, utf8, n);
    text[n] = 0;
    gCanvas->text(text, x, y, style(styleIndex), ink(token, 1.0f), align(alignment));
}

UI_EXPORT void ui_int(int value, float x, float y, int styleIndex, int token, int alignment)
{
    if (gCanvas == nullptr)
        return;
    char text[16];
    std::snprintf(text, sizeof text, "%d", value);
    gCanvas->text(text, x, y, style(styleIndex), ink(token, 1.0f), align(alignment));
}

UI_EXPORT float ui_cap_centre(float centreY, int styleIndex)
{
    return gCanvas != nullptr ? gCanvas->capCentreTop(centreY, style(styleIndex)) : centreY;
}

UI_EXPORT void ui_a11y_bytes(int id, int role, float x, float y, float w, float h, const void* title, int bytes,
                             float value, float lo, float hi)
{
    if (gItems == nullptr)
        return;
    funkgui::A11yItem item;
    item.id = static_cast<uint32_t>(id);
    item.role = static_cast<funkgui::A11yRole>(role);
    item.bounds = { x, y, w, h };
    item.title.assign(static_cast<const char*>(title), static_cast<size_t>(std::max(bytes, 0)));
    if (item.role == funkgui::A11yRole::slider)
    {
        item.v = value;
        item.lo = lo;
        item.hi = hi;
        item.step = 1;
        item.value = std::to_string(static_cast<int>(value));
    }
    gItems->push_back(std::move(item));
}

// ---- the program, as the host holds it ------------------------------------------------------------------------------

namespace
{
    struct App                                           // counter.fm's last lines, as C function pointers
    {
        int (*size)() = nullptr;
        int (*width)() = nullptr;
        int (*height)() = nullptr;
        void (*init)(void*) = nullptr;
        void (*relayout)(void*) = nullptr;
        void (*tick)(void*, float) = nullptr;
        bool (*moving)(void*) = nullptr;
        void (*draw)(void*) = nullptr;
        void (*pointer)(void*, float, float, int) = nullptr;
        bool (*key)(void*, int) = nullptr;
        void (*accessibility)(void*) = nullptr;
        void (*action)(void*, int, int, float) = nullptr;
        int (*count)(void*) = nullptr;
    };

    struct Instance                                      // zeroed, 64-byte aligned: all an FMinus module needs
    {
        explicit Instance(size_t bytes) : memory(std::aligned_alloc(64, (bytes + 63) / 64 * 64)) { std::memset(memory, 0, (bytes + 63) / 64 * 64); }
        ~Instance() { std::free(memory); }
        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        void* memory;
    };

    enum Phase { kMove, kDown, kDrag, kUp, kExit };      // ui.fm's enum Phase

    class FmPanel final : public funkgui::Panel
    {
    public:
        FmPanel(const App& app, void* instance) : app_(app), instance_(instance) {}

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return app_.width(); }
        int  height() const override { return app_.height(); }
        void tick(float dt) override { app_.tick(instance_, dt); }
        bool wantsFullRate() const override { return app_.moving(instance_); }
        void draw(funkgui::Canvas& canvas, const funkgui::Theme& theme) override
        {
            gCanvas = &canvas;
            gTheme = &theme;
            app_.draw(instance_);
            gCanvas = nullptr;
        }
        void pointerMove(const funkgui::PointerEvent& e) override { app_.pointer(instance_, e.x, e.y, kMove); }
        void pointerExit() override { app_.pointer(instance_, 0, 0, kExit); }
        void pointerDown(const funkgui::PointerEvent& e) override { app_.pointer(instance_, e.x, e.y, kDown); }
        void pointerDrag(const funkgui::PointerEvent& e) override { app_.pointer(instance_, e.x, e.y, kDrag); }
        void pointerUp(const funkgui::PointerEvent& e) override { app_.pointer(instance_, e.x, e.y, kUp); }
        bool key(const funkgui::KeyEvent& e) override { return app_.key(instance_, static_cast<int>(e.key)); }
        void accessibility(std::vector<funkgui::A11yItem>& items) const override
        {
            gItems = &items;
            app_.accessibility(instance_);
            gItems = nullptr;
        }
        uint32_t a11yRevision() const override { return 1; }
        void a11yAction(uint32_t id, funkgui::A11yAction action, double value) override
        {
            app_.action(instance_, static_cast<int>(id), static_cast<int>(action), static_cast<float>(value));
        }
        void closeGestures() override {}

    private:
        App   app_;
        void* instance_;
    };

    // ---- output ---------------------------------------------------------------------------------------------------

    void chunk(std::vector<uint8_t>& png, const char type[4], const uint8_t* data, size_t size)
    {
        const auto be32 = [&png](uint32_t v) { for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(v >> s)); };
        be32(static_cast<uint32_t>(size));
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data, data + size);
        be32(static_cast<uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(png.size() - start))));
    }

    bool writePng(const funkgui::Image& image, const std::string& path)   // FunkGui's own needs JUCE's encoder
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(image.h) * (static_cast<size_t>(image.w) * 4 + 1));
        for (int y = 0; y < image.h; ++y)
        {
            raw.push_back(0);
            const uint8_t* row = image.rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(image.w) * 4;
            raw.insert(raw.end(), row, row + static_cast<size_t>(image.w) * 4);
        }
        uLongf packedSize = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> packed(packedSize);
        if (compress2(packed.data(), &packedSize, raw.data(), static_cast<uLong>(raw.size()), 9) != Z_OK)
            return false;
        std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        const auto w = static_cast<uint32_t>(image.w), h = static_cast<uint32_t>(image.h);
        const uint8_t header[13] = { uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                                     uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 6, 0, 0, 0 };
        chunk(png, "IHDR", header, sizeof header);
        chunk(png, "IDAT", packed.data(), packedSize);
        chunk(png, "IEND", nullptr, 0);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        return out.good();
    }

    uint64_t fnv(const std::vector<uint8_t>& bytes)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (const uint8_t b : bytes)
            h = (h ^ b) * 0x100000001b3ull;
        return h;
    }

    double millisSince(std::chrono::steady_clock::time_point t)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    }

    // One frame of the host, reported: what was recorded, its fingerprint, and a picture of it.
    void report(funkgui::HeadlessHost& host, const std::string& name, const std::string& outDir)
    {
        const funkgui::PrimList& frame = host.draw();
        const funkgui::Fingerprint print = funkgui::fingerprint(frame);
        const funkgui::Image image = funkgui::rasterise(frame, funkgui::FontService::get().atlas());
        const bool wrote = writePng(image, outDir + "/" + name + ".png");
        std::printf("%-22s %3zu primitives, %d missing glyphs, geometry %016llx, text %016llx, %dx%d px %016llx%s\n",
                    name.c_str(), frame.prims.size(), frame.missingGlyphs,
                    static_cast<unsigned long long>(print.geometry), static_cast<unsigned long long>(print.text),
                    image.w, image.h, static_cast<unsigned long long>(fnv(image.rgba)), wrote ? "" : "  (no PNG written)");
    }

    // The script: what a person would do, through HeadlessHost's synthetic input. Positions are counter.fm's layout.
    void script(funkgui::HeadlessHost& host, const App& app, void* instance, const std::string& tag,
                const std::string& outDir)
    {
        const int settled = host.settle();
        report(host, tag + "-1-rest", outDir);

        host.click(128, 124);                            // "+", three times
        host.click(128, 124);
        host.click(128, 124);
        host.drag(24, 172, 24 + (360 - 48 - 56) * 0.5f, 172);   // the slider, to the middle of its track
        host.click(128, 124);                            // "+" again, now by the new step
        host.keys("up,up,down");                         // the arrow keys
        host.move(128, 124);                             // and rest on "+"
        host.tick(3);                                    // three frames on: the flash and the hover are mid-ease
        report(host, tag + "-2-moving", outDir);
        const int after = host.settle();
        report(host, tag + "-3-settled", outDir);
        std::printf("%-22s count %d; settled in %d frames at first, %d after the input\n", (tag + ":").c_str(),
                    app.count(instance), settled, after);

        for (const funkgui::A11yItem& item : host.accessibility())
            std::printf("  a11y  %s\n", funkgui::a11yDumpLine(item).c_str());

        // The cost of a frame: the Panel's draw into the Canvas (FMinus code calling the host for each primitive).
        const int frames = 20000;
        const auto start = std::chrono::steady_clock::now();
        size_t prims = 0;
        for (int i = 0; i < frames; ++i)
            prims += host.draw().prims.size();
        const double ms = millisSince(start);
        std::printf("%-22s %.2f us a frame recorded (%zu primitives), mean of %d\n", (tag + ":").c_str(),
                    ms * 1000.0 / frames, prims / frames, frames);
    }
}

#if APP_JIT

namespace
{
    std::string readFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    template <typename Signature>
    Signature* find(fminus::Program& program, const char* name)
    {
        std::string errors;
        auto* function = program.function<Signature>(name, errors);
        if (function == nullptr)
        {
            std::fprintf(stderr, "%s\n", errors.c_str());
            std::exit(1);
        }
        return function;
    }

    App bind(fminus::Program& program)
    {
        App app;
        app.size = find<int()>(program, "app_size");
        app.width = find<int()>(program, "app_width");
        app.height = find<int()>(program, "app_height");
        app.init = find<void(void*)>(program, "app_init");
        app.relayout = find<void(void*)>(program, "app_relayout");
        app.tick = find<void(void*, float)>(program, "app_tick");
        app.moving = find<bool(void*)>(program, "app_moving");
        app.draw = find<void(void*)>(program, "app_draw");
        app.pointer = find<void(void*, float, float, int)>(program, "app_pointer");
        app.key = find<bool(void*, int)>(program, "app_key");
        app.accessibility = find<void(void*)>(program, "app_accessibility");
        app.action = find<void(void*, int, int, float)>(program, "app_action");
        app.count = find<int(void*)>(program, "app_count");
        return app;
    }

    std::unique_ptr<fminus::Program> compile(const std::string& dir, bool edited, double& millis)
    {
        fminus::Program::Options options;
        options.fileName = "counter.fm";
        options.include = [dir](std::string_view name, std::string_view) -> std::optional<fminus::Program::IncludedFile> {
            return fminus::Program::IncludedFile{ std::string(name), readFile(dir + "/" + std::string(name)) };
        };
        if (edited)
            options.defines["V2"] = "1";
        const std::string source = readFile(dir + "/counter.fm");
        std::string errors;
        const auto start = std::chrono::steady_clock::now();
        auto program = fminus::Program::compile(source, errors, options);
        if (program != nullptr)
            static_cast<void>(bind(*program));           // the first look-up of a function generates the machine code
        millis = millisSince(start);
        if (program == nullptr)
        {
            std::fprintf(stderr, "%s\n", errors.c_str());
            std::exit(1);
        }
        return program;
    }

    // A reload's state: each field of the new module takes the value of the old module's field of the same name and
    // type, a nested module field by field; a field the edit added stays 0, and one it removed is dropped.
    int carryOver(const fminus::Program& oldProgram, const fminus::Program::Module& from, const char* source,
                  const fminus::Program& newProgram, const fminus::Program::Module& to, char* destination)
    {
        int carried = 0;
        for (const auto& field : to.fields)
        {
            const auto old = std::find_if(from.fields.begin(), from.fields.end(),
                                          [&field](const auto& f) { return f.name == field.name; });
            if (old == from.fields.end() || old->type != field.type)
                continue;
            const auto* oldModule = oldProgram.module(field.type);
            const auto* newModule = newProgram.module(field.type);
            if (oldModule != nullptr && newModule != nullptr)
                carried += carryOver(oldProgram, *oldModule, source + old->offset, newProgram, *newModule,
                                     destination + field.offset);
            else if (old->size == field.size)
            {
                std::memcpy(destination + field.offset, source + old->offset, field.size);
                ++carried;
            }
        }
        return carried;
    }
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: counter-jit <dir of counter.fm> <output dir>\n");
        return 2;
    }
    const std::string dir = argv[1], outDir = argv[2];
    const funkgui::HeadlessGuiScope gui;

    double millis = 0;
    auto program = compile(dir, false, millis);
    std::printf("jit:                   counter.fm compiled and linked in %.1f ms (-O2)\n", millis);
    App app = bind(*program);
    auto instance = std::make_unique<Instance>(static_cast<size_t>(app.size()));
    app.init(instance->memory);
    {
        FmPanel panel(app, instance->memory);
        funkgui::HeadlessHost host(panel);
        script(host, app, instance->memory, "jit", outDir);
    }

    // The edit, while the first program's state is live: compile, carry the state over, relay out, draw.
    auto edited = compile(dir, true, millis);
    const App next = bind(*edited);
    auto nextInstance = std::make_unique<Instance>(static_cast<size_t>(next.size()));
    const int carried = carryOver(*program, *program->module("App"), static_cast<const char*>(instance->memory),
                                  *edited, *edited->module("App"), static_cast<char*>(nextInstance->memory));
    next.relayout(nextInstance->memory);
    std::printf("reload:                the edit compiled in %.1f ms; App is %d bytes, was %d; %d fields carried over\n",
                millis, next.size(), app.size(), carried);
    {
        FmPanel panel(next, nextInstance->memory);
        funkgui::HeadlessHost host(panel);
        host.settle();
        report(host, "reload-1-carried", outDir);
        host.click(128, 124);                            // the edit's own state starts counting from here
        host.settle();
        report(host, "reload-2-used", outDir);
        std::printf("reload:                count %d after one more click\n", next.count(nextInstance->memory));
    }
    return 0;
}

#else   // ahead of time: counter.o is linked in, and these are its symbols

extern "C"
{
    int app_size();
    int app_width();
    int app_height();
    void app_init(void*);
    void app_relayout(void*);
    void app_tick(void*, float);
    bool app_moving(void*);
    void app_draw(void*);
    void app_pointer(void*, float, float, int);
    bool app_key(void*, int);
    void app_accessibility(void*);
    void app_action(void*, int, int, float);
    int app_count(void*);
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: counter-aot <output dir>\n");
        return 2;
    }
    const funkgui::HeadlessGuiScope gui;
    const App app{ app_size, app_width, app_height, app_init, app_relayout, app_tick, app_moving, app_draw,
                   app_pointer, app_key, app_accessibility, app_action, app_count };
    Instance instance(static_cast<size_t>(app.size()));
    app.init(instance.memory);
    FmPanel panel(app, instance.memory);
    funkgui::HeadlessHost host(panel);
    script(host, app, instance.memory, "aot", argv[1]);
    return 0;
}

#endif
