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
