// Build config files.
#include "checkmake.h"

#include <sstream>

// Config format, one setting per line:
//
//   # comment
//   output   = myprogram
//   compiler = g++
//   pubkey   = <64 hex chars>      (required: key that signs every dependency)
//   repo     = https://example.com/repo   (where packages come from)
//   package  = sodium ^1.0.22      (one per line; see versions.cpp for ranges)
//   packages = fmt@11.0.2 zlib@1.3 (exact versions, several per line)
//   platform = x86_64-linux-gnu-libstdc++ (optional: override detection)
//   sources  = main.cpp util.cpp
//   sources  = more.cpp            (repeat a key to append)
//   libs     = libfoo.a -lpthread
//   flags    = -O2 -Wall
//   pins     = build.pins          (optional: exact hash every dependency must match)
//
// Sources are looked up in src/, libs in lib/ (unless they start with "-",
// in which case they're passed to the compiler as-is, e.g. -lpthread).
bool loadConfig(const string &path, BuildConfig &config)
{
    config.path = path;
    vector<KeyValue> kvs;
    bool ok = parseKeyValueFile(path, kvs);
    if (kvs.empty() && !ok)
        return false;

    for (auto &&kv : kvs)
    {
        const string &key = kv.key, &value = kv.value;
        vector<string> words = splitWords(value);
        auto fail = [&](const string &msg)
        {
            cerr << path << ":" << kv.line << ": " << msg << endl;
            ok = false;
        };

        if (key == "sources" || key == "source")
        {
            for (auto &&w : words)
                config.sources.push_back("src/" + w);
        }
        else if (key == "libs" || key == "lib")
        {
            for (auto &&w : words)
                config.libs.push_back(w[0] == '-' ? w : LIB_DIR + "/" + w);
        }
        else if (key == "packages" || key == "package")
        {
            // Either "name@ver name@ver ..." or one "name <constraints>".
            vector<string> reqs;
            if (!words.empty() && words[0].find('@') != string::npos)
                reqs = words;
            else
                reqs = {value};
            for (auto &&r : reqs)
            {
                Requirement req;
                string err;
                if (!parseRequirement(r, req, err))
                    fail(err);
                else
                {
                    for (auto &&existing : config.packages)
                        if (existing.name == req.name)
                            fail("package '" + req.name + "' is listed twice");
                    config.packages.push_back(req);
                }
            }
        }
        else if (key == "repo")
        {
            config.repo = value;
            while (!config.repo.empty() && config.repo.back() == '/')
                config.repo.pop_back();
        }
        else if (key == "platform")
        {
            Platform p;
            if (!parsePlatform(value, p))
                fail("platform must look like x86_64-linux-gnu-libstdc++");
            config.platform = value;
        }
        else if (key == "pins")
            config.pins = value;
        else if (key == "flags")
            config.flags.insert(config.flags.end(), words.begin(), words.end());
        else if (key == "output")
            config.output = value;
        else if (key == "compiler")
            config.compiler = value;
        else if (key == "pubkey")
        {
            if (config.hasPubkey)
                fail("pubkey given more than once");
            else if (!hexToBin(value, config.pubkey, sizeof config.pubkey))
                fail("pubkey must be " + to_string(crypto_sign_PUBLICKEYBYTES * 2) + " hex characters");
            else
                config.hasPubkey = true;
        }
        else
            fail("unknown key '" + key + "'");
    }

    if (ok && config.sources.empty())
    {
        cerr << path << ": no sources listed" << endl;
        ok = false;
    }
    if (ok && !config.hasPubkey)
    {
        cerr << path << ": no pubkey given (needed to verify libs and includes)" << endl;
        ok = false;
    }
    return ok;
}

// Ask the compiler what it targets (e.g. x86_64-linux-gnu or
// x86_64-pc-linux-musl), and look at the flags for the C++ library.
bool detectPlatform(const BuildConfig &config, Platform &out)
{
    if (!config.platform.empty())
        return parsePlatform(config.platform, out);

    static map<string, Platform> cache;
    string cacheKey = config.compiler + "\n" + joinArgs(config.flags);
    if (cache.count(cacheKey))
    {
        out = cache[cacheKey];
        return true;
    }

    string triple;
    if (!captureCommand({config.compiler, "-dumpmachine"}, triple) || trim(triple).empty())
    {
        cerr << "Couldn't ask '" << config.compiler << " -dumpmachine' for the target platform"
             << " (set 'platform = ...' in the config instead)" << endl;
        return false;
    }
    triple = trim(triple);
    vector<string> parts = split(triple, '-');
    out.arch = parts[0];
    out.os = "linux";
    out.libc = triple.find("musl") != string::npos ? "musl" : triple.find("gnu") != string::npos ? "gnu" : "";
    out.cxx = "libstdc++";
    for (auto &&f : config.flags)
        if (f == "-stdlib=libc++")
            out.cxx = "libc++";
    if (triple.find("linux") == string::npos || out.libc.empty() || !isSafeName(out.arch))
    {
        cerr << "Unsupported target '" << triple << "' (only Linux with glibc or musl is supported)" << endl;
        return false;
    }
    cache[cacheKey] = out;
    return true;
}

string lockPathFor(const string &configPath)
{
    return siblingPath(configPath, ".lock");
}

string removePackageFromConfigText(const string &text, const string &name, bool &removed)
{
    removed = false;
    vector<string> lines = split(text, '\n');
    vector<string> kept;
    for (auto &&line : lines)
    {
        string content = trim(line.substr(0, line.find('#')));
        size_t eq = content.find('=');
        string key = eq == string::npos ? "" : trim(content.substr(0, eq));
        if (key != "package" && key != "packages")
        {
            kept.push_back(line);
            continue;
        }
        vector<string> words = splitWords(content.substr(eq + 1));
        if (words.empty())
        {
            kept.push_back(line);
            continue;
        }
        if (words[0].find('@') != string::npos)
        {
            vector<string> rest;
            for (auto &&w : words)
                if (w.substr(0, w.find('@')) != name)
                    rest.push_back(w);
            if (rest.size() == words.size())
                kept.push_back(line);
            else
            {
                removed = true;
                if (!rest.empty())
                    kept.push_back(key + " = " + join(rest, " "));
            }
        }
        else if (words[0] == name)
            removed = true;
        else
            kept.push_back(line);
    }
    return join(kept, "\n");
}
