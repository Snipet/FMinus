// src/frontend/Frontend.cpp
#include "frontend/Frontend.h"

#include "frontend/Scanner.h"

#include "frontend/Lexer.yy.hpp"   // the reentrant scanner's API: fminuslex_init_extra, fminus_scan_bytes, ...

#include <climits>
#include <stdexcept>

namespace fminus::frontend
{
    namespace
    {
        // A reentrant scanner reading `source` from memory, released however parsing ends.
        class ScannerHandle
        {
        public:
            ScannerHandle(std::string_view source, ScannerState& state)
            {
                if (fminuslex_init_extra(&state, &scanner_) != 0)
                    throw std::runtime_error("fminus: cannot create the scanner");
                buffer_ = fminus_scan_bytes(source.data(), static_cast<int>(source.size()), scanner_);
            }
            ~ScannerHandle()
            {
                fminus_delete_buffer(buffer_, scanner_);
                fminuslex_destroy(scanner_);
            }
            ScannerHandle(const ScannerHandle&) = delete;
            ScannerHandle& operator=(const ScannerHandle&) = delete;

            yyscan_t get() const
            {
                return scanner_;
            }

        private:
            yyscan_t scanner_ = nullptr;
            YY_BUFFER_STATE buffer_ = nullptr;
        };
    } // namespace

    std::optional<ast::Program> parse(std::string_view source, Diagnostics& diagnostics)
    {
        if (source.size() > static_cast<std::size_t>(INT_MAX))
        {
            diagnostics.error({}, "the source is too long");
            return std::nullopt;
        }
        ParseContext context{{}, diagnostics};
        ScannerState state;
        state.context = &context;
        ScannerHandle scanner(source, state);
        Parser parser(scanner.get(), context);
        if (parser.parse() != 0 || diagnostics.hasErrors())
            return std::nullopt;
        return std::move(context.program);
    }
} // namespace fminus::frontend
