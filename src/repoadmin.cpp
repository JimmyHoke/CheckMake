// Managing a repo folder on this machine:
//
//   checkmake repo init <key> <dir>                     start an empty, signed repo
//   checkmake repo list <dir> [--pubkey HEX]            every build in the repo
//   checkmake repo search <dir> <term> [--pubkey HEX]   builds whose name/description match
//   checkmake repo remove <key> <dir> <name>[@version] [--platform P] [-y]
//
// (Publishing is `checkmake pack`.) These work on the folder directly, so
// they don't need a project config. list/search check the manifest's
// signature when given --pubkey.
#include "checkmake.h"

#include <algorithm>
#include <unistd.h>

namespace
{
// Pull "--flag value" out of args; returns "" if absent.
string takeOption(vector<string> &args, const string &flag, bool &ok)
{
    for (size_t i = 0; i < args.size(); i++)
        if (args[i] == flag)
        {
            if (i + 1 >= args.size())
            {
                cerr << flag << " needs a value" << endl;
                ok = false;
                return "";
            }
            string v = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            return v;
        }
    return "";
}

bool takeFlag(vector<string> &args, const string &flag)
{
    auto it = find(args.begin(), args.end(), flag);
    if (it == args.end())
        return false;
    args.erase(it);
    return true;
}

void repoUsage()
{
    cerr << "Usage:\n"
         << "  checkmake repo init <secret-key-file> <repo-dir>\n"
         << "  checkmake repo list <repo-dir> [--pubkey HEX]\n"
         << "  checkmake repo search <repo-dir> <term> [--pubkey HEX]\n"
         << "  checkmake repo remove <secret-key-file> <repo-dir> <name>[@version] [--platform P] [-y]\n";
}

// Load a repo for reading, checking the signature if a public key is given.
bool loadForReading(const string &dir, const string &pubkeyHex, vector<PkgEntry> &entries)
{
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    bool havePk = !pubkeyHex.empty();
    if (havePk && !hexToBin(pubkeyHex, pk, sizeof pk))
    {
        cerr << "--pubkey must be " << crypto_sign_PUBLICKEYBYTES * 2 << " hex characters" << endl;
        return false;
    }
    bool exists;
    if (!loadLocalManifest(dir, havePk ? pk : nullptr, entries, exists))
        return false;
    if (!exists)
    {
        cerr << "'" << dir << "' isn't a repo (no manifest). Create one with 'checkmake repo init'." << endl;
        return false;
    }
    cout << (havePk ? "Manifest signature OK." : "(Manifest signature not checked; pass --pubkey to check it.)")
         << endl;
    return true;
}

// Show builds as a table, flagging any whose archive is missing, then any
// archives the manifest doesn't mention.
void showBuilds(const string &dir, const vector<PkgEntry> &builds, bool showStrays, const vector<PkgEntry> &all)
{
    vector<vector<string>> rows = {{"package", "version", "platform", "depends", "description"}};
    int missing = 0;
    set<string> names;
    for (auto &&e : builds)
    {
        names.insert(e.name);
        string desc = e.description;
        if (!fs::exists(fs::path(dir) / e.fileName()))
        {
            desc = "[ARCHIVE MISSING] " + desc;
            missing++;
        }
        rows.push_back({e.name, e.version, e.platform, e.deps.empty() ? "-" : formatDeps(e.deps), desc});
    }
    printTable(rows);
    cout << builds.size() << " build(s) of " << names.size() << " package(s)" << endl;
    if (missing)
        cerr << "warning: " << missing << " archive(s) listed in the manifest are missing from " << dir << endl;

    if (!showStrays)
        return;
    set<string> known;
    for (auto &&e : all)
        known.insert(e.fileName());
    for (auto &entry : fs::directory_iterator(dir))
    {
        string f = entry.path().filename().string();
        if (f.size() > PKG_EXT.size() && f.compare(f.size() - PKG_EXT.size(), PKG_EXT.size(), PKG_EXT) == 0 &&
            !known.count(f))
            cout << "note: " << f << " isn't in the manifest (not published)" << endl;
    }
}

// Could builds for these two platforms end up in the same program? True if
// either is "any", they're equal, or one is the C ABI form of the other
// (x86_64-linux-gnu vs x86_64-linux-gnu-libstdc++).
bool platformsCompatible(const string &a, const string &b)
{
    auto prefixOf = [](const string &p, const string &full) { return full.rfind(p + "-", 0) == 0; };
    return a == b || a == "any" || b == "any" || prefixOf(a, b) || prefixOf(b, a);
}

bool confirm(const string &question)
{
    if (!isatty(STDIN_FILENO))
    {
        cerr << "Not asking for confirmation without a terminal; pass -y to go ahead." << endl;
        return false;
    }
    cout << question << " [y/N] " << flush;
    string answer;
    getline(cin, answer);
    return answer == "y" || answer == "Y" || answer == "yes";
}

// ---------------------------------------------------------------------------

int repoInit(const string &keyPath, const string &dir)
{
    if (fs::exists(fs::path(dir) / MANIFEST))
    {
        cerr << "'" << dir << "' already has a manifest; it's already a repo" << endl;
        return 1;
    }
    unsigned char sk[crypto_sign_SECRETKEYBYTES], pk[crypto_sign_PUBLICKEYBYTES];
    if (!loadSecretKey(keyPath, sk))
        return 1;
    crypto_sign_ed25519_sk_to_pk(pk, sk);

    fs::create_directories(dir);
    bool ok = writeLocalManifest(dir, {}, sk);
    sodium_memzero(sk, sizeof sk);
    if (!ok)
        return 1;

    string abs = fs::absolute(dir).lexically_normal().generic_string();
    while (abs.size() > 1 && abs.back() == '/')
        abs.pop_back();
    cout << "Created an empty repo in " << abs << endl
         << "Publish packages to it with: checkmake pack " << keyPath << " <package-dir> " << dir << endl
         << "Projects use it with these config lines (or serve the folder over https and use that URL):" << endl
         << "  repo   = file://" << abs << endl
         << "  pubkey = " << binToHex(pk, sizeof pk) << endl;
    return 0;
}

int repoList(vector<string> args)
{
    bool ok = true;
    string pubkey = takeOption(args, "--pubkey", ok);
    if (!ok || args.size() != 1)
    {
        repoUsage();
        return 1;
    }
    vector<PkgEntry> entries;
    if (!loadForReading(args[0], pubkey, entries))
        return 1;
    if (entries.empty())
        cout << "The repo is empty." << endl;
    else
        showBuilds(args[0], entries, true, entries);
    return 0;
}

int repoSearch(vector<string> args)
{
    bool ok = true;
    string pubkey = takeOption(args, "--pubkey", ok);
    if (!ok || args.size() != 2)
    {
        repoUsage();
        return 1;
    }
    vector<PkgEntry> entries, matches;
    if (!loadForReading(args[0], pubkey, entries))
        return 1;
    string term = toLower(args[1]);
    for (auto &&e : entries)
        if (toLower(e.name).find(term) != string::npos || toLower(e.description).find(term) != string::npos)
            matches.push_back(e);
    if (matches.empty())
    {
        cout << "Nothing in the repo matches '" << args[1] << "'." << endl;
        return 0;
    }
    showBuilds(args[0], matches, false, entries);
    return 0;
}

int repoRemove(vector<string> args)
{
    bool ok = true;
    string platform = takeOption(args, "--platform", ok);
    bool yes = takeFlag(args, "-y") || takeFlag(args, "--yes");
    if (!ok || args.size() != 3)
    {
        repoUsage();
        return 1;
    }
    const string &keyPath = args[0], &dir = args[1], &what = args[2];
    string name = what.substr(0, what.find('@'));
    string version = what.find('@') == string::npos ? "" : what.substr(what.find('@') + 1);
    if (!isSafeName(name) || (what.find('@') != string::npos && !isSafeName(version)))
    {
        cerr << "Expected <name> or <name>@<version>, got '" << what << "'" << endl;
        return 1;
    }

    unsigned char sk[crypto_sign_SECRETKEYBYTES], pk[crypto_sign_PUBLICKEYBYTES];
    if (!loadSecretKey(keyPath, sk))
        return 1;
    crypto_sign_ed25519_sk_to_pk(pk, sk);

    // Refuse to re-sign a manifest that doesn't verify with this key.
    vector<PkgEntry> entries;
    bool exists;
    if (!loadLocalManifest(dir, pk, entries, exists) || !exists)
    {
        if (!exists)
            cerr << "'" << dir << "' isn't a repo (no manifest)" << endl;
        sodium_memzero(sk, sizeof sk);
        return 1;
    }

    vector<PkgEntry> keep, drop;
    for (auto &&e : entries)
    {
        bool match = e.name == name && (version.empty() || e.version == version) &&
                     (platform.empty() || e.platform == platform);
        (match ? drop : keep).push_back(e);
    }
    if (drop.empty())
    {
        cerr << "Nothing in the repo matches " << what << (platform.empty() ? "" : " for " + platform) << "." << endl;
        vector<string> have;
        for (auto &&e : entries)
            if (e.name == name)
                have.push_back(e.version + " (" + e.platform + ")");
        if (!have.empty())
            cerr << "Builds of " << name << ": " << join(have, ", ") << endl;
        sodium_memzero(sk, sizeof sk);
        return 1;
    }

    cout << "This will remove from " << dir << ":" << endl;
    for (auto &&e : drop)
        cout << "  " << e.id() << " (" << e.platform << ")  " << e.fileName() << endl;
    // Anything still in the repo that can only be satisfied by what's going away?
    for (auto &&k : keep)
        for (auto &&d : k.deps)
        {
            bool stillMet = any_of(keep.begin(), keep.end(), [&](const PkgEntry &o)
                                   { return o.name == d.name && satisfies(o.version, d) &&
                                            platformsCompatible(o.platform, k.platform); });
            if (!stillMet)
                cout << "warning: " << k.id() << " (" << k.platform << ") needs " << d.text()
                     << ", which nothing left in the repo provides" << endl;
        }
    cout << "Projects that have these versions locked will fail to download them." << endl;

    if (!yes && !confirm("Remove " + to_string(drop.size()) + " build(s)?"))
    {
        cout << "Nothing removed." << endl;
        sodium_memzero(sk, sizeof sk);
        return 1;
    }

    // Update the manifest first, so a failure part-way never leaves the
    // manifest pointing at a deleted archive.
    ok = writeLocalManifest(dir, keep, sk);
    sodium_memzero(sk, sizeof sk);
    if (!ok)
        return 1;
    for (auto &&e : drop)
    {
        error_code ec;
        fs::remove(fs::path(dir) / e.fileName(), ec);
        cout << "removed " << e.fileName() << endl;
    }
    cout << "Removed " << drop.size() << " build(s); " << keep.size() << " left in the repo." << endl;
    return 0;
}
} // namespace

int cmdRepo(const vector<string> &args)
{
    string sub = args.empty() ? "" : args[0];
    vector<string> rest = args.empty() ? args : vector<string>(args.begin() + 1, args.end());
    if (sub == "init" && rest.size() == 2)
        return repoInit(rest[0], rest[1]);
    if (sub == "list")
        return repoList(rest);
    if (sub == "search")
        return repoSearch(rest);
    if (sub == "remove")
        return repoRemove(rest);
    repoUsage();
    return sub == "-h" || sub == "--help" ? 0 : 1;
}
