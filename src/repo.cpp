// Packages: repo manifests, lock files, package metadata, dependency
// resolution, downloading, and publishing.
//
// A repo is a flat folder served at a URL (http, https or file):
//
//   manifest                              one package build per line (below)
//   manifest.sig                          signature over the manifest
//   sodium-1.0.22-2-x86_64-linux-gnu.tar.xz   a package
//
// Manifest (and lock file) lines:
//
//   name version platform blake2b-hash | dep; dep | description
//   e.g.  mid 2.0 x86_64-linux-gnu 61ab... | base ^2.0; hdr * | Middle layer
//
// A package archive holds lib/, include/, package.cm (its metadata) and an
// optional license.txt, every file with a .sig next to it.
//
// Downloads are checked twice: the archive's hash must match the signed
// manifest (or the lock file) before it's unpacked, and every file inside
// must carry a valid signature before it's used (and again on every build).
#include "checkmake.h"

#include <algorithm>
#include <functional>

// ---------------------------------------------------------------------------
// Manifest and lock file entries
// ---------------------------------------------------------------------------

string formatDeps(const vector<Requirement> &deps)
{
    vector<string> parts;
    for (auto &&d : deps)
        parts.push_back(d.text());
    return join(parts, "; ");
}

static string formatEntry(const PkgEntry &e, bool withDescription)
{
    string line = e.name + " " + e.version + " " + e.platform + " " + e.hash;
    bool desc = withDescription && !e.description.empty();
    if (!e.deps.empty() || desc)
        line += " | " + formatDeps(e.deps);
    if (desc)
        line += " | " + e.description;
    return line;
}

static bool parseEntry(const string &line, PkgEntry &e, string &err)
{
    vector<string> sections = split(line, '|');
    vector<string> head = splitWords(sections[0]);
    if (head.size() == 3)
    {
        err = "old-format line (name version hash); republish this repo with the current checkmake";
        return false;
    }
    if (head.size() != 4)
    {
        err = "expected 'name version platform hash | deps | description'";
        return false;
    }
    e = PkgEntry{};
    e.name = head[0];
    e.version = head[1];
    e.platform = head[2];
    e.hash = head[3];
    unsigned char tmp[HASH_BYTES];
    if (!isSafeName(e.name) || !isSafeName(e.version) || !isValidPackagePlatform(e.platform) ||
        !hexToBin(e.hash, tmp, sizeof tmp))
    {
        err = "bad name, version, platform or hash";
        return false;
    }
    if (sections.size() > 1)
        for (auto &&part : split(sections[1], ';'))
        {
            if (trim(part).empty())
                continue;
            Requirement req;
            if (!parseRequirement(part, req, err))
                return false;
            e.deps.push_back(req);
        }
    if (sections.size() > 2)
        e.description = trim(join(vector<string>(sections.begin() + 2, sections.end()), "|"));
    return true;
}

bool loadEntries(const string &path, vector<PkgEntry> &entries, const string &what)
{
    if (!fs::exists(path))
    {
        cerr << "Can't read " << what << " '" << path << "'" << endl;
        return false;
    }
    set<string> seen;
    int lineNum = 0;
    for (auto &&line : split(readFile(path), '\n'))
    {
        lineNum++;
        string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        PkgEntry e;
        string err;
        if (!parseEntry(t, e, err))
        {
            cerr << what << ":" << lineNum << ": " << err << endl;
            return false;
        }
        if (!seen.insert(e.key()).second)
        {
            cerr << what << ":" << lineNum << ": " << e.key() << " is listed twice" << endl;
            return false;
        }
        entries.push_back(e);
    }
    return true;
}

static void sortEntries(vector<PkgEntry> &entries)
{
    sort(entries.begin(), entries.end(), [](const PkgEntry &a, const PkgEntry &b)
         {
             if (a.name != b.name)
                 return a.name < b.name;
             int c = compareVersions(a.version, b.version);
             return c != 0 ? c < 0 : a.platform < b.platform;
         });
}

static string formatEntries(vector<PkgEntry> entries, const string &header, bool withDescription)
{
    sortEntries(entries);
    string text = header;
    for (auto &&e : entries)
        text += formatEntry(e, withDescription) + "\n";
    return text;
}

// ---------------------------------------------------------------------------
// package.cm
//
//   name        = mid
//   version     = 2.0
//   platform    = x86_64-linux-gnu      (or "any", or ...-libstdc++ / -libc++)
//   description = Middle layer
//   depends     = base ^2.0            (one per line, repeatable)
//   libs        = libmid.a              (link order; default: every file in lib/)
//   defines     = MID_STATIC            (passed as -D to anything using it)
//   syslibs     = pthread m             (system libraries: -lpthread -lm)
// ---------------------------------------------------------------------------

bool loadPackageMeta(const string &path, PackageMeta &meta)
{
    vector<KeyValue> kvs;
    bool ok = parseKeyValueFile(path, kvs);
    if (!ok && kvs.empty())
        return false;
    meta = PackageMeta{};
    for (auto &&kv : kvs)
    {
        auto fail = [&](const string &msg)
        {
            cerr << path << ":" << kv.line << ": " << msg << endl;
            ok = false;
        };
        vector<string> words = splitWords(kv.value);
        if (kv.key == "name")
            meta.name = kv.value;
        else if (kv.key == "version")
            meta.version = kv.value;
        else if (kv.key == "platform")
            meta.platform = kv.value;
        else if (kv.key == "description")
        {
            meta.description = kv.value;
            if (meta.description.find('|') != string::npos)
                fail("description can't contain '|'");
        }
        else if (kv.key == "depends")
        {
            Requirement req;
            string err;
            if (!parseRequirement(kv.value, req, err))
                fail(err);
            else
                meta.deps.push_back(req);
        }
        else if (kv.key == "libs")
        {
            for (auto &&w : words)
                if (!isSafeName(w))
                    fail("bad lib file name '" + w + "'");
            meta.libs.insert(meta.libs.end(), words.begin(), words.end());
        }
        else if (kv.key == "defines")
        {
            for (auto &&w : words)
                if (w[0] == '-' || w[0] == '=')
                    fail("bad define '" + w + "' (write NAME or NAME=VALUE)");
            meta.defines.insert(meta.defines.end(), words.begin(), words.end());
        }
        else if (kv.key == "syslibs")
        {
            for (auto &&w : words)
                if (!isSafeName(w))
                    fail("bad system library '" + w + "' (write e.g. pthread for -lpthread)");
            meta.syslibs.insert(meta.syslibs.end(), words.begin(), words.end());
        }
        else
            fail("unknown key '" + kv.key + "'");
    }
    if (!isSafeName(meta.name) || !isSafeName(meta.version) || !isdigit((unsigned char)meta.version[0]))
    {
        cerr << path << ": needs a valid name and version (version starting with a digit)" << endl;
        ok = false;
    }
    if (!isValidPackagePlatform(meta.platform))
    {
        cerr << path << ": platform must be 'any', e.g. x86_64-linux-gnu (C), or x86_64-linux-gnu-libstdc++ (C++)"
             << endl;
        ok = false;
    }
    return ok;
}

vector<string> packageLibFiles(const ResolvedPackage &pkg)
{
    vector<string> libs;
    fs::path dir = fs::path(pkg.entry.dir()) / LIB_DIR;
    if (!pkg.meta.libs.empty())
    {
        for (auto &&l : pkg.meta.libs)
            libs.push_back((dir / l).generic_string());
        return libs;
    }
    if (fs::is_directory(dir))
        for (auto &entry : fs::directory_iterator(dir))
            if (entry.is_regular_file() && entry.path().extension() != SIG_EXT)
                libs.push_back(entry.path().generic_string());
    sort(libs.begin(), libs.end());
    return libs;
}

// ---------------------------------------------------------------------------
// Downloading
// ---------------------------------------------------------------------------

static bool download(const string &url, const string &out)
{
    // -f: fail on HTTP errors instead of saving the error page.
    return runCommand({"curl", "-fsSL", "--proto", "=http,https,file", "-o", out, url}, false) == 0;
}

bool fetchManifest(const BuildConfig &config, vector<PkgEntry> &entries)
{
    static map<string, vector<PkgEntry>> cache; // one fetch per repo per run
    if (cache.count(config.repo))
    {
        entries = cache[config.repo];
        return true;
    }
    if (config.repo.empty())
    {
        cerr << "No repo is set in " << config.path << " (add 'repo = <url>')" << endl;
        return false;
    }

    fs::create_directories(DOWNLOAD_DIR);
    string path = DOWNLOAD_DIR + "/" + MANIFEST;
    cout << "Fetching package list from " << config.repo << endl;
    if (!download(config.repo + "/" + MANIFEST, path) || !download(config.repo + "/" + MANIFEST + SIG_EXT, path + SIG_EXT))
    {
        cerr << "Couldn't download the repo manifest" << endl;
        return false;
    }
    string why;
    if (!verifyFile(path, MANIFEST, config.pubkey, why))
    {
        cerr << "Repo manifest failed verification: " << why << endl;
        return false;
    }
    if (!loadEntries(path, entries, "manifest"))
        return false;
    cache[config.repo] = entries;
    return true;
}

static bool installPackage(const BuildConfig &config, const PkgEntry &e)
{
    if (config.repo.empty())
    {
        cerr << e.id() << " isn't in " << DEPS_DIR << "/ and no repo is set in " << config.path << endl;
        return false;
    }
    fs::create_directories(DOWNLOAD_DIR);
    string archive = DOWNLOAD_DIR + "/" + e.fileName();
    cout << "  downloading " << e.fileName() << endl;
    string hash;
    if (!download(config.repo + "/" + e.fileName(), archive))
    {
        cerr << "Couldn't download " << e.fileName() << endl;
        return false;
    }
    if (!hashFile(archive, hash) || hash != e.hash)
    {
        cerr << e.fileName() << " doesn't match the hash in the signed manifest / lock file" << endl;
        return false;
    }

    fs::path extracted = fs::path(DOWNLOAD_DIR) / fs::path(e.dir()).filename();
    fs::remove_all(extracted);
    fs::create_directories(extracted);
    if (runCommand({"tar", "--no-same-owner", "--no-same-permissions", "-I", "pixz", "-xf", archive, "-C",
                    extracted.string()},
                   false) != 0)
    {
        cerr << "Couldn't unpack " << e.fileName() << endl;
        return false;
    }
    int bad = 0;
    verifyTree(extracted, "", config.pubkey, bad);
    if (bad > 0 || !fs::exists(extracted / PACKAGE_META))
    {
        cerr << e.id() << " contains badly signed files or no " << PACKAGE_META << endl;
        return false;
    }
    // Only move it into place once it's fully verified.
    fs::create_directories(DEPS_DIR);
    fs::rename(extracted, e.dir());
    fs::remove(archive);
    return true;
}

// Load a package's metadata, checking its signature and that it agrees with
// the manifest/lock entry it was resolved from.
static bool loadInstalledMeta(const BuildConfig &config, const PkgEntry &e, PackageMeta &meta)
{
    string path = e.dir() + "/" + PACKAGE_META;
    string why;
    if (!verifyFile(path, PACKAGE_META, config.pubkey, why))
    {
        cerr << path << ": " << why << endl;
        return false;
    }
    if (!loadPackageMeta(path, meta))
        return false;
    if (meta.name != e.name || meta.version != e.version || meta.platform != e.platform)
    {
        cerr << path << " says " << meta.name << "@" << meta.version << "/" << meta.platform << ", expected "
             << e.key() << endl;
        return false;
    }
    if (formatDeps(meta.deps) != formatDeps(e.deps))
    {
        cerr << e.id() << ": dependencies in " << PACKAGE_META << " (" << formatDeps(meta.deps)
             << ") don't match the manifest (" << formatDeps(e.deps) << ")" << endl;
        return false;
    }
    for (auto &&l : meta.libs)
        if (!fs::is_regular_file(e.dir() + "/" + LIB_DIR + "/" + l))
        {
            cerr << e.id() << ": lists lib/" << l << " but doesn't contain it" << endl;
            return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
// Dependency resolution
//
// A backtracking search: take a package that something requires but that
// isn't chosen yet, try its candidate builds best-first (preferred/locked
// version, then newest, then most platform-specific), add that build's own
// requirements, and back up if anything conflicts.
// ---------------------------------------------------------------------------

namespace
{
struct SolveState
{
    map<string, const PkgEntry *> chosen;
    map<string, vector<pair<Requirement, string>>> reqs; // name -> (requirement, who needs it)
};

struct Solver
{
    map<string, vector<const PkgEntry *>> candidates;
    string platformName;
    string error;
    long steps = 0;

    bool solve(const SolveState &st, map<string, const PkgEntry *> &out)
    {
        if (++steps > 200000)
        {
            error = "dependency resolution gave up: too many combinations to try";
            return false;
        }
        string target;
        for (auto &&[name, rs] : st.reqs)
            if (!st.chosen.count(name))
            {
                target = name;
                break;
            }
        if (target.empty())
        {
            out = st.chosen;
            return true;
        }

        const auto &needs = st.reqs.at(target);
        auto it = candidates.find(target);
        if (it == candidates.end() || it->second.empty())
        {
            if (error.empty())
            {
                error = "no package '" + target + "' is available for " + platformName + " (needed by";
                for (auto &&[req, from] : needs)
                    error += " " + from;
                error += ")";
            }
            return false;
        }

        bool anyFit = false;
        for (const PkgEntry *c : it->second)
        {
            bool fits = true;
            for (auto &&[req, from] : needs)
                fits = fits && satisfies(c->version, req);
            if (!fits)
                continue;
            anyFit = true;

            SolveState next = st;
            next.chosen[target] = c;
            bool conflict = false;
            for (auto &&d : c->deps)
            {
                next.reqs[d.name].push_back({d, c->id()});
                auto ch = next.chosen.find(d.name);
                if (ch != next.chosen.end() && !satisfies(ch->second->version, d))
                {
                    conflict = true;
                    if (error.empty())
                    {
                        error = "conflicting requirements for '" + d.name + "':";
                        for (auto &&[req, from] : next.reqs[d.name])
                            error += "\n    " + req.text() + "   (from " + from + ")";
                    }
                }
            }
            if (!conflict && solve(next, out))
                return true;
        }

        if (!anyFit && error.empty())
        {
            error = "no version of '" + target + "' satisfies all of:";
            for (auto &&[req, from] : needs)
                error += "\n    " + req.text() + "   (from " + from + ")";
            set<string> versions;
            for (auto *c : it->second)
                versions.insert(c->version);
            error += "\n  available for " + platformName + ": " + join(vector<string>(versions.begin(), versions.end()), ", ");
        }
        return false;
    }
};
} // namespace

bool resolvePackages(const BuildConfig &config, const Platform &platform, const vector<PkgEntry> &candidates,
                     const map<string, string> &prefer, map<string, PkgEntry> &chosen, string &err)
{
    Solver solver;
    solver.platformName = platform.full();
    for (auto &&e : candidates)
        if (platformMatch(e.platform, platform) > 0)
            solver.candidates[e.name].push_back(&e);

    for (auto &&[name, list] : solver.candidates)
    {
        auto pref = prefer.find(name);
        sort(list.begin(), list.end(), [&](const PkgEntry *a, const PkgEntry *b)
             {
                 if (pref != prefer.end())
                 {
                     bool pa = a->version == pref->second, pb = b->version == pref->second;
                     if (pa != pb)
                         return pa;
                 }
                 int c = compareVersions(a->version, b->version);
                 if (c != 0)
                     return c > 0;
                 return platformMatch(a->platform, platform) > platformMatch(b->platform, platform);
             });
    }

    SolveState start;
    for (auto &&r : config.packages)
        start.reqs[r.name].push_back({r, config.path});

    map<string, const PkgEntry *> out;
    if (!solver.solve(start, out))
    {
        err = solver.error;
        return false;
    }
    chosen.clear();
    for (auto &&[name, e] : out)
        chosen[name] = *e;
    return true;
}

// ---------------------------------------------------------------------------
// Sync: resolve -> lock -> install -> load metadata
// ---------------------------------------------------------------------------

static void reportChanges(const vector<PkgEntry> &before, const map<string, PkgEntry> &after)
{
    map<string, PkgEntry> old;
    for (auto &&e : before)
        old[e.name] = e;
    for (auto &&[name, e] : after)
    {
        auto it = old.find(name);
        if (it == old.end())
            cout << "  + " << name << " " << e.version << endl;
        else if (it->second.version != e.version)
            cout << "  ~ " << name << " " << it->second.version << " -> " << e.version << endl;
        else if (it->second.platform != e.platform)
            cout << "  ~ " << name << " " << e.version << " build " << it->second.platform << " -> " << e.platform
                 << endl;
    }
    for (auto &&[name, e] : old)
        if (!after.count(name))
            cout << "  - " << name << " " << e.version << endl;
}

bool syncPackages(const BuildConfig &config, Resolution &out, SyncMode mode, const set<string> &names)
{
    out.clear();
    string lockPath = lockPathFor(config.path);
    vector<PkgEntry> locked;
    bool haveLock = fs::exists(lockPath);
    if (haveLock && !loadEntries(lockPath, locked, lockPath))
        return false;

    map<string, PkgEntry> chosen;
    if (!config.packages.empty())
    {
        Platform platform;
        if (!detectPlatform(config, platform))
            return false;

        map<string, string> prefer;
        for (auto &&e : locked)
            if (mode == SyncMode::Normal || (!names.empty() && !names.count(e.name)))
                prefer[e.name] = e.version;

        // Usually the lock file already answers everything: no network needed.
        string err;
        bool solved = mode == SyncMode::Normal && haveLock &&
                      resolvePackages(config, platform, locked, prefer, chosen, err);
        if (!solved)
        {
            vector<PkgEntry> candidates;
            if (!fetchManifest(config, candidates))
                return false;
            // Keep locked builds available even if the repo dropped them, but
            // refuse if the repo now serves different bytes under the same name.
            map<string, const PkgEntry *> byKey;
            for (auto &&c : candidates)
                byKey[c.key()] = &c;
            vector<PkgEntry> extra;
            for (auto &&l : locked)
            {
                auto it = byKey.find(l.key());
                if (it == byKey.end())
                    extra.push_back(l);
                else if (it->second->hash != l.hash)
                {
                    cerr << "The repo's " << l.key() << " doesn't match the hash in " << lockPath
                         << ". A published package changed; refusing to continue." << endl;
                    return false;
                }
            }
            candidates.insert(candidates.end(), extra.begin(), extra.end());

            err.clear();
            cout << "Resolving packages for " << platform.full() << endl;
            if (!resolvePackages(config, platform, candidates, prefer, chosen, err))
            {
                cerr << "Can't resolve packages: " << err << endl;
                return false;
            }
        }
    }

    // Write the lock file if anything changed.
    vector<PkgEntry> lockEntries;
    for (auto &&[name, e] : chosen)
        lockEntries.push_back(e);
    string lockText = formatEntries(lockEntries,
                                    "# checkmake lock file for " + config.path +
                                        ": the exact package builds in use. Commit it.\n"
                                        "# Change it with: checkmake add / remove / update\n",
                                    false);
    if ((haveLock || !chosen.empty()) && readFile(lockPath) != lockText)
    {
        reportChanges(locked, chosen);
        if (!writeFile(lockPath, lockText))
        {
            cerr << "Can't write " << lockPath << endl;
            return false;
        }
        cout << "Updated " << lockPath << endl;
    }

    // Install anything missing.
    bool ok = true;
    for (auto &&[name, e] : chosen)
        if (ok && !fs::exists(e.dir()))
        {
            ok = installPackage(config, e);
            if (ok)
                cout << "  installed " << e.id() << " (" << e.platform << ")" << endl;
        }
    error_code ec;
    fs::remove_all(DOWNLOAD_DIR, ec);
    if (!ok)
        return false;

    // Load metadata and order for linking: dependents before dependencies.
    map<string, PackageMeta> metas;
    for (auto &&[name, e] : chosen)
        if (!loadInstalledMeta(config, e, metas[name]))
            return false;

    vector<string> order;
    set<string> visited;
    function<void(const string &)> visit = [&](const string &name)
    {
        if (!visited.insert(name).second)
            return;
        for (auto &&d : metas[name].deps)
            visit(d.name);
        order.push_back(name);
    };
    set<string> direct;
    for (auto &&r : config.packages)
    {
        direct.insert(r.name);
        visit(r.name);
    }
    reverse(order.begin(), order.end());
    for (auto &&name : order)
        out.push_back({chosen[name], metas[name], direct.count(name) > 0});
    return true;
}

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

bool loadLocalManifest(const string &repoDir, const unsigned char *pubkey, vector<PkgEntry> &entries, bool &exists)
{
    string path = (fs::path(repoDir) / MANIFEST).string();
    exists = fs::exists(path);
    if (!exists)
        return true;
    if (pubkey)
    {
        string why;
        if (!verifyFile(path, MANIFEST, pubkey, why))
        {
            cerr << path << " failed verification with this key: " << why << endl;
            return false;
        }
    }
    return loadEntries(path, entries, path);
}

bool writeLocalManifest(const string &repoDir, const vector<PkgEntry> &entries, const unsigned char *secretKey)
{
    string path = (fs::path(repoDir) / MANIFEST).string();
    string text = formatEntries(entries,
                                "# checkmake repo manifest\n"
                                "# name version platform blake2b-256 | depends | description\n",
                                true);
    if (!writeFile(path, text))
    {
        cerr << "Can't write " << path << endl;
        return false;
    }
    return signFileAs(path, MANIFEST, secretKey);
}

// checkmake pack <secret-key-file> <package-dir> <repo-dir>
//
// Reads <package-dir>/package.cm, signs every file in lib/, include/,
// license.txt and package.cm, bundles them into the repo, adds the build to
// the repo manifest and re-signs the manifest.
int cmdPack(const string &keyPath, const string &pkgDir, const string &repoDir)
{
    PackageMeta meta;
    fs::path metaPath = fs::path(pkgDir) / PACKAGE_META;
    if (!fs::is_regular_file(metaPath))
    {
        cerr << "'" << pkgDir << "' has no " << PACKAGE_META << endl;
        return 1;
    }
    if (!loadPackageMeta(metaPath.string(), meta))
        return 1;
    for (auto &&l : meta.libs)
        if (!fs::is_regular_file(fs::path(pkgDir) / LIB_DIR / l))
        {
            cerr << PACKAGE_META << " lists lib/" << l << " but it doesn't exist" << endl;
            return 1;
        }

    vector<string> parts;
    for (auto &&d : {LIB_DIR, INCLUDE_DIR})
        if (fs::is_directory(fs::path(pkgDir) / d))
            parts.push_back(d);
    if (parts.empty())
    {
        cerr << "'" << pkgDir << "' has no lib/ or include/ folder" << endl;
        return 1;
    }
    if (meta.platform != "any" && !fs::is_directory(fs::path(pkgDir) / LIB_DIR))
        cout << "note: no lib/ folder; header-only packages can use 'platform = any'" << endl;

    PkgEntry entry{meta.name, meta.version, meta.platform, "", meta.description, meta.deps};
    fs::create_directories(repoDir);
    fs::path archive = fs::path(repoDir) / entry.fileName();
    if (fs::exists(archive))
    {
        cerr << archive.string() << " already exists; publish a new version instead" << endl;
        return 1;
    }

    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    if (!loadSecretKey(keyPath, sk))
        return 1;
    crypto_sign_ed25519_sk_to_pk(pk, sk);

    // Load the current manifest, refusing to re-sign one that's been tampered with.
    vector<PkgEntry> manifest;
    bool exists;
    bool ok = loadLocalManifest(repoDir, pk, manifest, exists);
    for (auto &&m : manifest)
        if (ok && m.key() == entry.key())
        {
            cerr << entry.key() << " is already in the manifest" << endl;
            ok = false;
        }

    for (auto &&d : parts)
        if (ok)
            ok = signTree(pkgDir, d, sk);

    for (auto &&file : {LICENSE_FILE, PACKAGE_META})
    {
        fs::path p = fs::path(pkgDir) / file;
        if (fs::is_symlink(p))
        {
            cerr << "Refusing to pack symlink " << p.string() << endl;
            ok = false;
        }
        else if (fs::is_regular_file(p))
        {
            if (ok)
                ok = signFileAs(p.string(), file, sk);
            parts.push_back(file);
            parts.push_back(file + SIG_EXT);
        }
        else if (file == LICENSE_FILE)
            cout << "note: no " << LICENSE_FILE << " in " << pkgDir << "; packing without a license" << endl;
    }

    if (ok)
    {
        vector<string> tarCmd = {"tar", "--owner=0", "--group=0", "--numeric-owner", "--sort=name",
                                 "-I", "pixz", "-cf", archive.string(), "-C", pkgDir};
        tarCmd.insert(tarCmd.end(), parts.begin(), parts.end());
        if (runCommand(tarCmd, false) != 0)
        {
            cerr << "Failed to create " << archive.string() << endl;
            fs::remove(archive);
            ok = false;
        }
    }
    if (ok && !hashFile(archive.string(), entry.hash))
        ok = false;

    if (ok)
    {
        manifest.push_back(entry);
        ok = writeLocalManifest(repoDir, manifest, sk);
    }

    sodium_memzero(sk, sizeof sk);
    if (ok)
        cout << "Published " << entry.id() << " (" << entry.platform << ") to " << repoDir << endl;
    return ok ? 0 : 1;
}
