#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "sparenode/foundation/text/diagnostic_escape.hpp"

TEST_CASE("Diagnostic escaping preserves its path and log contracts", "[foundation][text]")
{
    std::ostringstream path;
    sparenode::text::append_safe_diagnostic(path, "a\\b\n\x7f",
                                            sparenode::text::DiagnosticEscapeStyle::path);
    CHECK(path.str() == "a\\b\\x0A\\x7F");

    std::ostringstream log;
    sparenode::text::append_safe_diagnostic(log, "a\\b\n\t\x01",
                                            sparenode::text::DiagnosticEscapeStyle::log);
    CHECK(log.str() == "a\\\\b\\n\\t\\x01");
}

TEST_CASE("Diagnostic escaping blocks UTF-8 C1 terminal controls", "[foundation][text]")
{
    const std::string_view control = "\xC2\x9B"
                                     "2J";
    for (const auto style : {sparenode::text::DiagnosticEscapeStyle::path,
                             sparenode::text::DiagnosticEscapeStyle::log})
    {
        std::ostringstream output;
        sparenode::text::append_safe_diagnostic(output, control, style);
        CHECK(output.str() == "\\xC2\\x9B2J");
    }

    std::ostringstream unicode;
    sparenode::text::append_safe_diagnostic(unicode, "\xC3\xA9\xE0\xA0\x80",
                                            sparenode::text::DiagnosticEscapeStyle::log);
    CHECK(unicode.str() == "\xC3\xA9\xE0\xA0\x80");

    std::ostringstream malformed;
    sparenode::text::append_safe_diagnostic(malformed,
                                            "\x9B"
                                            "2J",
                                            sparenode::text::DiagnosticEscapeStyle::log);
    CHECK(malformed.str() == "\\x9B2J");
}
