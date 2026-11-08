// Versions, version ranges and platforms.
#include "checkmake.h"

// ---------------------------------------------------------------------------
// Versions
//
// A version is split into runs of digits and runs of letters; everything
// else just separates them. Segments compare left to right: numbers
// numerically, letters alphabetically, a number beats letters, and a longer
// version beats its own prefix. So 1.9 < 1.10 and 1.0.22 < 1.0.22-1 (a
// package revision).
// ---------------------------------------------------------------------------

static vector<string> versionSegments(const string &v)
{
    vector<string> segs;
    string cur;
    for (char c : v)
    {
        bool digit = isdigit((unsigned char)c), alpha = isalpha((unsigned char)c);
        if (!digit && !alpha)
        {
            if (!cur.empty())
                segs.push_back(cur);
            cur.clear();
            continue;
        }
        if (!cur.empty() && (isdigit((unsigned char)cur[0]) != 0) != digit)
        {
            segs.push_back(cur);
            cur.clear();
        }
        cur += c;
    }
    if (!cur.empty())
        segs.push_back(cur);
    return segs;
}

static int compareNumeric(string a, string b)
{
    a.erase(0, min(a.find_first_not_of('0'), a.size()));
    b.erase(0, min(b.find_first_not_of('0'), b.size()));
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    return a.compare(b) < 0 ? -1 : a.compare(b) > 0 ? 1 : 0;
}

int compareVersions(const string &a, const string &b)
{
    vector<string> sa = versionSegments(a), sb = versionSegments(b);
    for (size_t i = 0; i < max(sa.size(), sb.size()); i++)
    {
        if (i >= sa.size())
            return -1;
        if (i >= sb.size())
            return 1;
        bool da = isdigit((unsigned char)sa[i][0]), db = isdigit((unsigned char)sb[i][0]);
        int c;
        if (da && db)
            c = compareNumeric(sa[i], sb[i]);
        else if (da != db)
            c = da ? 1 : -1;
        else
            c = sa[i] < sb[i] ? -1 : sa[i] > sb[i] ? 1 : 0;
        if (c != 0)
            return c;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Requirements
//
//   name               any version
//   name 1.2.3         exactly 1.2.3 (same as =1.2.3, or name@1.2.3)
//   name ^1.2.3        >=1.2.3 <2       (^0.3.1 -> <0.4, ^0.0.3 -> <0.0.4)
//   name ~1.2.3        >=1.2.3 <1.3
//   name >=1.2 <2      several constraints must all hold (commas optional)
//   name *             any version
// ---------------------------------------------------------------------------

// The numeric x.y.z part of a version, ignoring any "-revision".
static bool numericParts(const string &v, vector<long> &parts)
{
    string base = v.substr(0, v.find_first_of("-+"));
    for (auto &&p : split(base, '.'))
    {
        if (p.empty() || p.find_first_not_of("0123456789") != string::npos || p.size() > 9)
            return false;
        parts.push_back(stol(p));
    }
    return !parts.empty();
}

static string joinParts(const vector<long> &parts)
{
    vector<string> s;
    for (long p : parts)
        s.push_back(to_string(p));
    return join(s, ".");
}

static bool parseConstraint(const string &tok, vector<Constraint> &out, string &err)
{
    static const vector<string> ops = {">=", "<=", ">", "<", "=", "^", "~"};
    if (tok == "*")
        return true;
    string op = "=", ver = tok;
    for (auto &&o : ops)
        if (tok.rfind(o, 0) == 0)
        {
            op = o;
            ver = tok.substr(o.size());
            break;
        }
    if (!isSafeName(ver) || !isdigit((unsigned char)ver[0]))
    {
        err = "bad version constraint '" + tok + "'";
        return false;
    }

    if (op == "^" || op == "~")
    {
        vector<long> parts;
        if (!numericParts(ver, parts))
        {
            err = "'" + tok + "' needs a numeric version like 1.2.3";
            return false;
        }
        vector<long> upper;
        if (op == "^")
        {
            // Bump the first non-zero part: ^1.2.3 -> 2, ^0.2.3 -> 0.3, ^0.0.3 -> 0.0.4.
            size_t i = 0;
            while (i + 1 < parts.size() && parts[i] == 0)
                i++;
            upper.assign(parts.begin(), parts.begin() + i + 1);
        }
        else
        {
            // ~1.2.3 -> 1.3, ~1 -> 2.
            upper.assign(parts.begin(), parts.begin() + min<size_t>(2, parts.size()));
        }
        upper.back()++;
        out.push_back({">=", ver});
        out.push_back({"<", joinParts(upper)});
        return true;
    }
    out.push_back({op, ver});
    return true;
}

bool parseRequirement(const string &textIn, Requirement &req, string &err)
{
    string text = textIn;
    for (char &c : text)
        if (c == ',')
            c = ' ';
    vector<string> words = splitWords(text);
    if (words.empty())
    {
        err = "empty package requirement";
        return false;
    }

    req = Requirement{};
    size_t at = words[0].find('@');
    if (at != string::npos)
    {
        // name@version: exact, the original pinned form.
        if (words.size() > 1)
        {
            err = "use either name@version or name <constraints>, not both";
            return false;
        }
        req.name = words[0].substr(0, at);
        words = {"=" + words[0].substr(at + 1)};
    }
    else
    {
        req.name = words[0];
        words.erase(words.begin());
    }
    if (!isSafeName(req.name))
    {
        err = "bad package name '" + req.name + "'";
        return false;
    }
    for (auto &&w : words)
        if (!parseConstraint(w, req.constraints, err))
            return false;
    req.spec = join(words, " ");
    if (req.spec == "*")
        req.spec = "";
    return true;
}

bool satisfies(const string &version, const Requirement &req)
{
    for (auto &&c : req.constraints)
    {
        int cmp = compareVersions(version, c.version);
        bool ok = (c.op == "=" && cmp == 0) || (c.op == ">" && cmp > 0) || (c.op == ">=" && cmp >= 0) ||
                  (c.op == "<" && cmp < 0) || (c.op == "<=" && cmp <= 0);
        if (!ok)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Platforms
//
// Static libraries only work on the platform they were built for. A target
// is <arch>-linux-<libc>-<c++ library>, e.g. x86_64-linux-gnu-libstdc++.
// A package says what it needs:
//   any                        header-only, works everywhere
//   x86_64-linux-gnu           plain C ABI: any C++ standard library is fine
//   x86_64-linux-gnu-libstdc++ uses C++ ABI: must match exactly
// ---------------------------------------------------------------------------

static bool validLibc(const string &s) { return s == "gnu" || s == "musl"; }
static bool validCxx(const string &s) { return s == "libstdc++" || s == "libc++"; }

bool parsePlatform(const string &p, Platform &out)
{
    // libstdc++ / libc++ contain no '-', so splitting on '-' is safe.
    vector<string> parts = split(p, '-');
    if (parts.size() != 4 || !isSafeName(parts[0]) || parts[1] != "linux" || !validLibc(parts[2]) ||
        !validCxx(parts[3]))
        return false;
    out = {parts[0], parts[1], parts[2], parts[3]};
    return true;
}

bool isValidPackagePlatform(const string &p)
{
    if (p == "any")
        return true;
    vector<string> parts = split(p, '-');
    if (parts.size() < 3 || parts.size() > 4)
        return false;
    if (!isSafeName(parts[0]) || parts[1] != "linux" || !validLibc(parts[2]))
        return false;
    return parts.size() == 3 || validCxx(parts[3]);
}

int platformMatch(const string &pkgPlatform, const Platform &target)
{
    if (pkgPlatform == "any")
        return 1;
    if (pkgPlatform == target.base())
        return 2;
    if (pkgPlatform == target.full())
        return 3;
    return 0;
}
