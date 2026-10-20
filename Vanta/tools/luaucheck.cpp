//
//  luaucheck.cpp
//  nexa-luaucheck - the editor's error checker.
//
//  Answers one question, exactly: would the executor accept this script? The
//  Luau it parses and compiles with is the Luau in src/luau, driven with the
//  options in CompileOptions.h, which is the pair the dylib loads scripts
//  with at run time. So a diagnostic from here is a script that cannot run,
//  and silence from here is a script that can.
//
//  It says nothing about types, nothing about names and nothing about style.
//  The game a script is written for is not on this machine - `workspace.Thing`
//  is unknowable and always will be - and an executor script reaches for
//  globals another script injected. Everything above the parser lives in the
//  editor, in JavaScript, where the Roblox tables are.
//
//  Two modes:
//      nexa-luaucheck --file script.lua     one answer, then exit
//      nexa-luaucheck                       CHECK <id> <len>\n<bytes>, forever
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Luau/Ast.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Compiler.h"
#include "Luau/Parser.h"

#include "CompileOptions.h"

namespace
{

// A file with more errors than this is being typed, not debugged.
constexpr size_t kMaxDiagnostics = 20;

struct Diagnostic
{
    unsigned line = 1;       // 1-based, as Monaco counts
    unsigned column = 1;     // 1-based, in UTF-16 code units, as Monaco counts
    unsigned endLine = 1;
    unsigned endColumn = 1;
    std::string message;
};

/// Byte offset of the start of every line, so a (line, byteColumn) from Luau
/// can be turned into the column Monaco means.
std::vector<size_t> lineStarts(const std::string& source)
{
    std::vector<size_t> starts{0};
    for (size_t i = 0; i < source.size(); ++i)
        if (source[i] == '\n')
            starts.push_back(i + 1);
    return starts;
}

/// Luau counts bytes within a line; Monaco counts UTF-16 code units. A script
/// with an emoji in a string earlier on the line would otherwise put the
/// squiggle in the wrong place, and executor scripts are full of them.
unsigned utf16Column(const std::string& source, const std::vector<size_t>& starts, unsigned line, unsigned byteColumn)
{
    if (line >= starts.size())
        return byteColumn + 1;

    size_t begin = starts[line];
    size_t end = (line + 1 < starts.size()) ? starts[line + 1] : source.size();
    size_t stop = begin + byteColumn;
    if (stop > end)
        stop = end;

    unsigned units = 0;
    for (size_t i = begin; i < stop; ++i)
    {
        unsigned char c = static_cast<unsigned char>(source[i]);
        if ((c & 0xC0) == 0x80)
            continue;                    // a continuation byte is not a unit
        units += (c >= 0xF0) ? 2 : 1;    // an astral code point is a surrogate pair
    }
    return units + 1;
}

/// JSON, with ill-formed UTF-8 replaced rather than passed through. A parse
/// error quotes the token it choked on, and that token can be any bytes at
/// all - one invalid byte would otherwise make the whole reply unparseable.
void writeJsonString(std::string& out, const std::string& text)
{
    out += '"';
    size_t i = 0;
    while (i < text.size())
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char>(c);
            ++i;
        }
        else if (c == '\n') { out += "\\n"; ++i; }
        else if (c == '\r') { out += "\\r"; ++i; }
        else if (c == '\t') { out += "\\t"; ++i; }
        else if (c < 0x20)
        {
            char buffer[8];
            snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
            ++i;
        }
        else if (c < 0x80)
        {
            out += static_cast<char>(c);
            ++i;
        }
        else
        {
            // How many continuation bytes this lead byte promises, and whether
            // they are actually there.
            size_t want = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
            bool ok = want > 0 && i + want < text.size();
            for (size_t k = 1; ok && k <= want; ++k)
                ok = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
            if (ok)
            {
                out.append(text, i, want + 1);
                i += want + 1;
            }
            else
            {
                out += "\xEF\xBF\xBD";   // U+FFFD
                ++i;
            }
        }
    }
    out += '"';
}

std::vector<Diagnostic> check(const std::string& source)
{
    std::vector<Diagnostic> out;
    std::vector<size_t> starts = lineStarts(source);

    auto add = [&](const Luau::Location& at, const std::string& message) {
        if (out.size() >= kMaxDiagnostics)
            return;
        Diagnostic d;
        d.line = at.begin.line + 1;
        d.column = utf16Column(source, starts, at.begin.line, at.begin.column);
        d.endLine = at.end.line + 1;
        d.endColumn = utf16Column(source, starts, at.end.line, at.end.column);
        // A zero-width range is an invisible squiggle.
        if (d.endLine == d.line && d.endColumn <= d.column)
            d.endColumn = d.column + 1;
        d.message = message;
        out.push_back(d);
    };

    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);

    // parse() does not throw: a fatal error is appended to the list it returns.
    // So this reports every error the parser recovered from, not just the first.
    Luau::ParseResult parsed =
        Luau::Parser::parse(source.data(), source.size(), names, allocator, Luau::ParseOptions{});

    for (const Luau::ParseError& error : parsed.errors)
        add(error.getLocation(), error.getMessage());

    if (!out.empty() || parsed.root == nullptr)
        return out;

    // Everything the parser accepts but the compiler does not: more than 200
    // locals in a function, the upvalue limit, a constant table that will not
    // fit. The compiler stops at the first, so there is at most one.
    try
    {
        Luau::BytecodeBuilder bytecode;
        Luau::compileOrThrow(bytecode, parsed, names, vantaCompileOptions());
    }
    catch (const Luau::CompileError& error)
    {
        add(error.getLocation(), error.what());
    }
    catch (...)
    {
        // Something the compiler did not name. Reporting it at a position we do
        // not know would be a squiggle in the wrong place, which is worse than
        // saying nothing at all.
    }

    return out;
}

std::string reply(long id, const std::vector<Diagnostic>& diagnostics)
{
    std::string out = "{\"id\":" + std::to_string(id) + ",\"errors\":[";
    for (size_t i = 0; i < diagnostics.size(); ++i)
    {
        const Diagnostic& d = diagnostics[i];
        if (i)
            out += ',';
        out += "{\"line\":" + std::to_string(d.line) + ",\"column\":" + std::to_string(d.column) +
               ",\"endLine\":" + std::to_string(d.endLine) + ",\"endColumn\":" + std::to_string(d.endColumn) +
               ",\"message\":";
        writeJsonString(out, d.message);
        out += '}';
    }
    out += "]}";
    return out;
}

bool readExactly(std::string& into, size_t count)
{
    into.resize(count);
    size_t got = 0;
    while (got < count)
    {
        size_t n = fread(&into[got], 1, count - got, stdin);
        if (n == 0)
            return false;
        got += n;
    }
    return true;
}

std::string readWholeFile(FILE* file)
{
    std::string out;
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0)
        out.append(buffer, n);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "--file") == 0)
    {
        FILE* file = fopen(argv[2], "rb");
        if (!file)
        {
            fprintf(stderr, "nexa-luaucheck: cannot open %s\n", argv[2]);
            return 2;
        }
        std::string source = readWholeFile(file);
        fclose(file);

        printf("%s\n", reply(0, check(source)).c_str());
        return 0;
    }

    // The editor's mode: one process for the session, one request at a time.
    // Framed by byte length rather than by a delimiter, so the source is passed
    // through untouched - no escaping, no line-ending games, and no scan for a
    // terminator that could appear in a string literal.
    for (;;)
    {
        char header[64];
        if (!fgets(header, sizeof(header), stdin))
            return 0;

        long id = 0;
        long long length = -1;
        if (sscanf(header, "CHECK %ld %lld", &id, &length) != 2 || length < 0)
            return 1;

        std::string source;
        if (length > 0 && !readExactly(source, static_cast<size_t>(length)))
            return 1;

        std::string out = reply(id, check(source));
        out += '\n';
        fwrite(out.data(), 1, out.size(), stdout);
        fflush(stdout);
    }
}
