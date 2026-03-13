#pragma once
// =============================================================================
// ingamefm_patch_serializer.h  — part of ingamefm (header-only)
//
// Serialize a YM2612Patch to C++ source code and parse it back.
// No external dependencies beyond the C++ standard library.
//
// Usage:
//   std::string code = IngameFMSerializer::serialize(patch, "MY_PATCH", 0, 0, 0);
//
//   YM2612Patch out; int block, lfoEn, lfoFreq;
//   std::string err; int errLine, errCol;
//   bool ok = IngameFMSerializer::parse(code, out, block, lfoEn, lfoFreq,
//                                       err, errLine, errCol);
// =============================================================================

#include "ingamefm_patchlib.h"

#include <string>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cctype>

struct IngameFMSerializer
{
    // -------------------------------------------------------------------------
    // Serialize
    // -------------------------------------------------------------------------
    static std::string serialize(const YM2612Patch& patch,
                                 const std::string& name,
                                 int block      = 0,
                                 int lfoEnable  = 0,
                                 int lfoFreq    = 0)
    {
        std::ostringstream o;
        o << "constexpr YM2612Patch " << name << " =\n"
          << "{\n"
          << "    .ALG = " << patch.ALG << ",\n"
          << "    .FB  = " << patch.FB  << ",\n"
          << "    .AMS = " << patch.AMS << ",\n"
          << "    .FMS = " << patch.FMS << ",\n"
          << "\n"
          << "    .op =\n"
          << "    {\n";
        for (int i = 0; i < 4; ++i)
        {
            const auto& op = patch.op[i];
            o << "        { "
              << ".DT = "  << op.DT  << ", "
              << ".MUL = " << op.MUL << ", "
              << ".TL = "  << op.TL  << ", "
              << ".RS = "  << op.RS  << ", "
              << ".AR = "  << op.AR  << ", "
              << ".AM = "  << op.AM  << ", "
              << ".DR = "  << op.DR  << ", "
              << ".SR = "  << op.SR  << ", "
              << ".SL = "  << op.SL  << ", "
              << ".RR = "  << op.RR  << ", "
              << ".SSG = " << op.SSG
              << " }";
            if (i < 3) o << ",";
            o << "\n";
        }
        o << "    }\n"
          << "};\n"
          << "constexpr int " << name << "_BLOCK = "      << block     << ";      // Octave offset\n"
          << "constexpr int " << name << "_LFO_ENABLE = " << lfoEnable << "; // LFO on/off\n"
          << "constexpr int " << name << "_LFO_FREQ = "   << lfoFreq   << ";   // LFO frequency (0-7)";
        return o.str();
    }

    // -------------------------------------------------------------------------
    // Parse
    // Returns true on success.  On failure, error/errLine/errCol are set.
    // errLine and errCol are 1-based.
    // -------------------------------------------------------------------------
    static bool parse(const std::string& code,
                      YM2612Patch& outPatch,
                      int& outBlock,
                      int& outLfoEnable,
                      int& outLfoFreq,
                      std::string& error,
                      int& errLine,
                      int& errCol)
    {
        outBlock = outLfoEnable = outLfoFreq = 0;
        errLine  = 1;
        errCol   = 1;

        bool foundALG = false, foundFB  = false;
        bool foundAMS = false, foundFMS = false;
        bool inOpArray = false;
        int  opCount   = 0;

        // Split into lines
        std::vector<std::string> lines;
        {
            std::istringstream ss(code);
            std::string ln;
            while (std::getline(ss, ln)) lines.push_back(ln);
        }

        for (int li = 0; li < (int)lines.size(); ++li)
        {
            errLine = li + 1;
            errCol  = 1;
            std::string line = trim(lines[li]);

            // Skip blanks and comments
            if (line.empty() || startsWith(line, "//")) continue;

            // Skip structural tokens
            if (startsWith(line, "constexpr") && contains(line, "YM2612Patch")) continue;
            if (line == "{" || line == "};") continue;

            // _BLOCK / _LFO_ENABLE / _LFO_FREQ constants
            if (contains(line, "_BLOCK") && !contains(line, "YM2612"))
            {
                int v;
                if (!parseConstInt(line, v, error, errCol)) return false;
                outBlock = v;
                continue;
            }
            if (contains(line, "_LFO_ENABLE"))
            {
                int v;
                if (!parseConstInt(line, v, error, errCol)) return false;
                outLfoEnable = (v != 0) ? 1 : 0;
                continue;
            }
            if (contains(line, "_LFO_FREQ"))
            {
                int v;
                if (!parseConstInt(line, v, error, errCol)) return false;
                if (v < 0 || v > 7)
                { error = "_LFO_FREQ must be 0-7"; return false; }
                outLfoFreq = v;
                continue;
            }

            // Global fields
            if (startsWith(line, ".ALG"))
            {
                if (!parseIntField(line, ".ALG", 0, 7, outPatch.ALG, error, errCol)) return false;
                foundALG = true; continue;
            }
            if (startsWith(line, ".FB"))
            {
                if (!parseIntField(line, ".FB", 0, 7, outPatch.FB, error, errCol)) return false;
                foundFB = true; continue;
            }
            if (startsWith(line, ".AMS"))
            {
                if (!parseIntField(line, ".AMS", 0, 3, outPatch.AMS, error, errCol)) return false;
                foundAMS = true; continue;
            }
            if (startsWith(line, ".FMS"))
            {
                if (!parseIntField(line, ".FMS", 0, 7, outPatch.FMS, error, errCol)) return false;
                foundFMS = true; continue;
            }

            // .op = section opener
            if (startsWith(line, ".op"))
            { inOpArray = true; continue; }

            // Operator line: contains both { and }
            if (inOpArray && contains(line, "{") && contains(line, "}"))
            {
                if (opCount >= 4)
                { error = "Too many operators (expected 4)"; return false; }
                if (!parseOperator(line, outPatch.op[opCount], error, errCol))
                    return false;
                ++opCount;
                continue;
            }
        }

        // Validate required fields
        if (!foundALG)    { error = ".ALG field required";   return false; }
        if (!foundFB)     { error = ".FB field required";    return false; }
        if (!foundAMS)    { error = ".AMS field required";   return false; }
        if (!foundFMS)    { error = ".FMS field required";   return false; }
        if (!inOpArray)   { error = ".op array required";    return false; }
        if (opCount != 4)
        {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "Expected 4 operators, found %d", opCount);
            error = buf;
            return false;
        }
        return true;
    }

private:
    // ── String utilities (no <algorithm> needed) ──────────────────────────────

    static std::string trim(const std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    static bool startsWith(const std::string& s, const std::string& prefix)
    {
        return s.size() >= prefix.size() &&
               s.compare(0, prefix.size(), prefix) == 0;
    }

    static bool contains(const std::string& s, const std::string& needle)
    {
        return s.find(needle) != std::string::npos;
    }

    // Split string by any char in `delims`
    static std::vector<std::string> splitBy(const std::string& s,
                                            const std::string& delims)
    {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s)
        {
            if (delims.find(c) != std::string::npos)
            { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    // ── Parsers ───────────────────────────────────────────────────────────────

    // Parse "constexpr int NAME = VALUE;" → VALUE
    static bool parseConstInt(const std::string& line, int& out,
                               std::string& error, int& errCol)
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        { error = "Expected '=' in constant definition"; errCol = (int)line.size(); return false; }
        std::string valStr = trim(line.substr(eq + 1));
        // Strip trailing ';' and comment
        size_t sc = valStr.find(';');
        if (sc != std::string::npos) valStr = valStr.substr(0, sc);
        valStr = trim(valStr);
        if (valStr.empty() || !isValidInt(valStr))
        { error = "Invalid integer value"; errCol = (int)eq + 1; return false; }
        out = std::stoi(valStr);
        return true;
    }

    // Parse ".FIELD = VALUE," or ".FIELD = VALUE\n" → VALUE with range check
    static bool parseIntField(const std::string& line,
                               const std::string& fieldName,
                               int lo, int hi, int& out,
                               std::string& error, int& errCol)
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        { error = fieldName + " missing '='"; errCol = (int)line.size(); return false; }
        std::string valStr = trim(line.substr(eq + 1));
        // Strip trailing comma/semicolon
        while (!valStr.empty() && (valStr.back() == ',' || valStr.back() == ';'))
            valStr.pop_back();
        valStr = trim(valStr);
        if (valStr.empty() || !isValidInt(valStr))
        { error = fieldName + ": invalid integer"; errCol = (int)eq + 1; return false; }
        int v = std::stoi(valStr);
        if (v < lo || v > hi)
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s must be %d-%d, got %d",
                     fieldName.c_str(), lo, hi, v);
            error = buf;
            errCol = (int)eq + 1;
            return false;
        }
        out = v;
        return true;
    }

    // Parse "{ .DT = V, .MUL = V, ... }" into YM2612Operator
    static bool parseOperator(const std::string& line, YM2612Operator& op,
                               std::string& error, int& errCol)
    {
        size_t lb = line.find('{');
        size_t rb = line.find('}');
        if (lb == std::string::npos || rb == std::string::npos || rb <= lb)
        { error = "Operator must be enclosed in { }"; return false; }

        std::string content = line.substr(lb + 1, rb - lb - 1);

        // Defaults
        op = {};

        // Split by commas, then parse each ".FIELD = VALUE"
        auto assignments = splitBy(content, ",");
        for (auto& asgn : assignments)
        {
            std::string a = trim(asgn);
            if (a.empty()) continue;
            size_t eq = a.find('=');
            if (eq == std::string::npos) continue;
            std::string prop = trim(a.substr(0, eq));
            std::string vstr = trim(a.substr(eq + 1));
            if (vstr.empty()) continue;
            if (!isValidInt(vstr))
            {
                error = "Invalid value for " + prop + ": '" + vstr + "'";
                errCol = (int)line.find(vstr) + 1;
                return false;
            }
            int v = std::stoi(vstr);

            if      (prop == ".DT")  op.DT  = v;
            else if (prop == ".MUL") op.MUL = v;
            else if (prop == ".TL")  op.TL  = v;
            else if (prop == ".RS")  op.RS  = v;
            else if (prop == ".AR")  op.AR  = v;
            else if (prop == ".AM")  op.AM  = v;
            else if (prop == ".DR")  op.DR  = v;
            else if (prop == ".SR")  op.SR  = v;
            else if (prop == ".SL")  op.SL  = v;
            else if (prop == ".RR")  op.RR  = v;
            else if (prop == ".SSG") op.SSG = v;
            else
            {
                error = "Unknown operator property: " + prop;
                errCol = (int)line.find(prop) + 1;
                return false;
            }
        }
        return true;
    }

    // Returns true if s is a valid integer literal (optional leading minus, then digits)
    static bool isValidInt(const std::string& s)
    {
        if (s.empty()) return false;
        size_t start = (s[0] == '-') ? 1 : 0;
        if (start == s.size()) return false;
        for (size_t i = start; i < s.size(); ++i)
            if (!isdigit((unsigned char)s[i])) return false;
        return true;
    }
};
