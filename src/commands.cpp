// Package management commands: pin, prune, add, remove, update, outdated,
// list, search, info.
#include "checkmake.h"

#include <algorithm>
#include <functional>

static bool loadLock(const BuildConfig &config, vector<PkgEntry> &locked, bool &haveLock)
{
    string path = lockPathFor(config.path);
    haveLock = fs::exists(path);
    return !haveLock || loadEntries(path, locked, path);
}

static void pinsReminder(const BuildConfig &config)
{
    if (!config.pins.empty())
        cout << "Hash pins are on: the next build will fail until you review the change and run "
                "'checkmake pin'."
             << endl;
}

// Rewrite the config, re-resolve, and put the old config back if that fails.
static int applyConfigChange(const string &configPath, const string &original, const string &updated)
{
    if (!writeFile(configPath, updated))
    {
        cerr << "Can't write " << configPath << endl;
        return 1;
    }
    BuildConfig config;
    Resolution res;
    if (!loadConfig(configPath, config) || !syncPackages(config, res))
    {
        writeFile(configPath, original);
        cerr << "Left " << configPath << " unchanged." << endl;
        return 1;
    }
    pinsReminder(config);
    return 0;
}

// ---------------------------------------------------------------------------

// checkmake pin [config]
//
// Records the hash of every current dependency (after checking signatures)
// and turns pinning on in the config if it isn't already.
int cmdPin(const string &configPath)
{
    BuildConfig config;
    if (!loadConfig(configPath, config))
        return 1;
    Resolution res;
    if (!syncPackages(config, res))
        return 1;

    Pins hashes;
    if (!verifyDependencies(config, res, &hashes))
        return 1;

    bool enable = config.pins.empty();
    string pinPath = enable ? siblingPath(configPath, ".pins") : config.pins;
    if (!writePins(pinPath, hashes))
        return 1;
    cout << "Pinned " << hashes.size() << " file(s) in " << pinPath << endl;

    if (enable)
    {
        string text = readFile(configPath);
        if (!text.empty() && text.back() != '\n')
            text += "\n";
        if (!writeFile(configPath, text + "pins     = " + pinPath + "\n"))
        {
            cerr << "Couldn't add 'pins = " << pinPath << "' to " << configPath << endl;
            return 1;
        }
        cout << "Enabled pinning in " << configPath << endl;
    }
    return 0;
}

// checkmake prune [config...]
//
// Delete package folders in deps/ that none of the given configs' lock files
// use (e.g. old versions after an update), and their copied licenses.
int cmdPrune(const vector<string> &configPaths)
{
    set<string> keepDirs, keepLicenses;
    for (auto &&path : configPaths)
    {
        BuildConfig config;
        vector<PkgEntry> locked;
        bool haveLock;
        if (!loadConfig(path, config) || !loadLock(config, locked, haveLock))
            return 1;
        if (!haveLock && !config.packages.empty())
        {
            cerr << path << " has no lock file yet; build it first so prune knows what it uses" << endl;
            return 1;
        }
        for (auto &&e : locked)
        {
            keepDirs.insert(fs::path(e.dir()).filename().string());
            keepLicenses.insert(e.name + "-" + e.version + ".txt");
        }
    }

    if (!fs::is_directory(DEPS_DIR))
        return 0;
    int removed = 0;
    for (auto &entry : fs::directory_iterator(DEPS_DIR))
    {
        if (keepDirs.count(entry.path().filename().string()))
            continue;
        PackageMeta meta;
        fs::path metaPath = entry.path() / PACKAGE_META;
        bool haveMeta = fs::exists(metaPath) && loadPackageMeta(metaPath.string(), meta);
        fs::remove_all(entry.path());
        cout << "removed " << entry.path().generic_string() << endl;
        removed++;
        if (haveMeta)
        {
            string lic = meta.name + "-" + meta.version + ".txt";
            fs::path licPath = fs::path(LICENSES_DIR) / lic;
            if (!keepLicenses.count(lic) && fs::exists(licPath))
            {
                fs::remove(licPath);
                cout << "removed " << licPath.generic_string() << endl;
            }
        }
    }
    cout << "Pruned " << removed << " folder(s); kept " << keepDirs.size() << " package(s)" << endl;
    return 0;
}

// Newest build of `name` usable on `platform`, or nullptr.
static const PkgEntry *newestFor(const vector<PkgEntry> &entries, const string &name, const Platform &platform)
{
    const PkgEntry *best = nullptr;
    for (auto &&e : entries)
        if (e.name == name && platformMatch(e.platform, platform) > 0 &&
            (!best || compareVersions(e.version, best->version) > 0))
            best = &e;
    return best;
}

// checkmake add <name> [constraints...]   e.g. add zlib ^1.3, add fmt
//
// With no constraint, adds "^<newest version>". Re-adding a package
// replaces its constraint.
int cmdAdd(const string &configPath, const vector<string> &args)
{
    BuildConfig config;
    if (!loadConfig(configPath, config))
        return 1;

    string reqText = join(args, " ");
    Requirement req;
    string err;
    if (!parseRequirement(reqText, req, err))
    {
        cerr << err << endl;
        return 1;
    }

    if (req.constraints.empty())
    {
        Platform platform;
        vector<PkgEntry> manifest;
        if (!detectPlatform(config, platform) || !fetchManifest(config, manifest))
            return 1;
        const PkgEntry *newest = newestFor(manifest, req.name, platform);
        if (!newest)
        {
            bool exists = any_of(manifest.begin(), manifest.end(), [&](auto &e) { return e.name == req.name; });
            if (exists)
                cerr << "'" << req.name << "' has no build for " << platform.full() << endl;
            else
                cerr << "No package named '" << req.name << "' in the repo (try 'checkmake search')" << endl;
            return 1;
        }
        // ^ needs a numeric version; fall back to >= for odd ones.
        reqText = req.name + " ^" + newest->version;
        if (!parseRequirement(reqText, req, err))
            reqText = req.name + " >=" + newest->version;
    }

    string original = readFile(configPath);
    bool replaced;
    string text = removePackageFromConfigText(original, req.name, replaced);
    if (!text.empty() && text.back() != '\n')
        text += "\n";
    text += "package  = " + reqText + "\n";

    cout << (replaced ? "Changing " : "Adding ") << reqText << " in " << configPath << endl;
    return applyConfigChange(configPath, original, text);
}

// checkmake remove <name>...
int cmdRemove(const string &configPath, const vector<string> &names)
{
    BuildConfig config;
    vector<PkgEntry> locked;
    bool haveLock;
    if (!loadConfig(configPath, config) || !loadLock(config, locked, haveLock))
        return 1;

    string original = readFile(configPath), text = original;
    for (auto &&name : names)
    {
        bool removed;
        text = removePackageFromConfigText(text, name, removed);
        if (!removed)
        {
            vector<string> neededBy;
            for (auto &&e : locked)
                for (auto &&d : e.deps)
                    if (d.name == name)
                        neededBy.push_back(e.name);
            if (!neededBy.empty())
                cerr << "'" << name << "' isn't listed in " << configPath << "; it's pulled in by "
                     << join(neededBy, ", ") << endl;
            else
                cerr << "'" << name << "' isn't a package in " << configPath << endl;
            return 1;
        }
        cout << "Removing " << name << " from " << configPath << endl;
    }
    int rc = applyConfigChange(configPath, original, text);
    if (rc == 0)
        cout << "Run 'checkmake prune' to delete package folders that are no longer used." << endl;
    return rc;
}

// checkmake update [name...]: move to the newest versions the config allows.
int cmdUpdate(const string &configPath, const vector<string> &names)
{
    BuildConfig config;
    vector<PkgEntry> locked;
    bool haveLock;
    if (!loadConfig(configPath, config) || !loadLock(config, locked, haveLock))
        return 1;

    for (auto &&n : names)
    {
        bool known = any_of(config.packages.begin(), config.packages.end(), [&](auto &r) { return r.name == n; }) ||
                     any_of(locked.begin(), locked.end(), [&](auto &e) { return e.name == n; });
        if (!known)
        {
            cerr << "'" << n << "' isn't one of this project's packages" << endl;
            return 1;
        }
    }

    string before = readFile(lockPathFor(configPath));
    Resolution res;
    if (!syncPackages(config, res, SyncMode::Update, set<string>(names.begin(), names.end())))
        return 1;
    if (readFile(lockPathFor(configPath)) == before)
        cout << "Everything is already at the newest version the config allows." << endl;
    else
    {
        cout << "Run 'checkmake prune' to delete the old versions' folders." << endl;
        pinsReminder(config);
    }
    return 0;
}

// checkmake outdated: compare locked versions with what's available.
int cmdOutdated(const string &configPath)
{
    BuildConfig config;
    vector<PkgEntry> locked;
    bool haveLock;
    if (!loadConfig(configPath, config) || !loadLock(config, locked, haveLock))
        return 1;
    if (config.packages.empty())
    {
        cout << "No packages." << endl;
        return 0;
    }
    if (!haveLock)
    {
        cerr << "No lock file yet; run a build first." << endl;
        return 1;
    }

    Platform platform;
    vector<PkgEntry> manifest;
    if (!detectPlatform(config, platform) || !fetchManifest(config, manifest))
        return 1;
    vector<PkgEntry> candidates = manifest;
    for (auto &&l : locked)
        if (none_of(manifest.begin(), manifest.end(), [&](auto &m) { return m.key() == l.key(); }))
            candidates.push_back(l);

    map<string, PkgEntry> wanted;
    string err;
    if (!resolvePackages(config, platform, candidates, {}, wanted, err))
    {
        cerr << "Can't resolve the newest allowed versions: " << err << endl;
        return 1;
    }

    map<string, string> current;
    for (auto &&e : locked)
        current[e.name] = e.version;
    set<string> names;
    for (auto &&[n, v] : current)
        names.insert(n);
    for (auto &&[n, e] : wanted)
        names.insert(n);

    vector<vector<string>> rows = {{"package", "current", "wanted", "latest", ""}};
    bool newerThanAllowed = false;
    for (auto &&n : names)
    {
        string cur = current.count(n) ? current[n] : "-";
        string want = wanted.count(n) ? wanted[n].version : "-";
        const PkgEntry *newest = newestFor(manifest, n, platform);
        string latest = newest ? newest->version : "-";
        if (cur == want && cur == latest)
            continue;
        newerThanAllowed = newerThanAllowed || want != latest;
        bool direct = any_of(config.packages.begin(), config.packages.end(), [&](auto &r) { return r.name == n; });
        rows.push_back({n, cur, want, latest, direct ? "" : "(indirect)"});
    }
    if (rows.size() == 1)
    {
        cout << "All packages are up to date." << endl;
        return 0;
    }
    printTable(rows);
    cout << "'wanted' is the newest version " << configPath << " allows: get it with 'checkmake update'." << endl;
    if (newerThanAllowed)
        cout << "'latest' is newer still: change the version range with 'checkmake add <name>' to move to it." << endl;
    return 0;
}

// checkmake list: the locked packages as a dependency tree. No network.
int cmdList(const string &configPath)
{
    BuildConfig config;
    vector<PkgEntry> locked;
    bool haveLock;
    if (!loadConfig(configPath, config) || !loadLock(config, locked, haveLock))
        return 1;
    if (config.packages.empty())
    {
        cout << "No packages." << endl;
        return 0;
    }
    if (!haveLock)
    {
        cerr << "No lock file yet; run a build first." << endl;
        return 1;
    }

    map<string, const PkgEntry *> byName;
    for (auto &&e : locked)
        byName[e.name] = &e;

    set<string> expanded;
    function<void(const string &, const string &, const string &, bool, bool)> show =
        [&](const string &name, const string &why, const string &prefix, bool last, bool root)
    {
        auto it = byName.find(name);
        string branch = root ? "" : (last ? "└─ " : "├─ ");
        if (it == byName.end())
        {
            cout << prefix << branch << name << "  (not in lock file)" << endl;
            return;
        }
        const PkgEntry &e = *it->second;
        string line = prefix + branch + e.name + " " + e.version + "  " + e.platform;
        if (!why.empty())
            line += "  [" + why + "]";
        if (!fs::exists(e.dir()))
            line += "  (not installed)";
        bool repeat = !expanded.insert(name).second && !e.deps.empty();
        cout << line << (repeat ? "  (deps shown above)" : "") << endl;
        if (repeat)
            return;
        string childPrefix = root ? "" : prefix + (last ? "   " : "│  ");
        for (size_t i = 0; i < e.deps.size(); i++)
            show(e.deps[i].name, e.deps[i].spec, childPrefix, i + 1 == e.deps.size(), false);
    };
    for (auto &&r : config.packages)
        show(r.name, r.spec, "", true, true);
    return 0;
}

// checkmake search [term]
int cmdSearch(const string &configPath, const string &term)
{
    BuildConfig config;
    Platform platform;
    vector<PkgEntry> manifest;
    if (!loadConfig(configPath, config) || !detectPlatform(config, platform) || !fetchManifest(config, manifest))
        return 1;

    map<string, const PkgEntry *> newestAny;
    for (auto &&e : manifest)
        if (!newestAny.count(e.name) || compareVersions(e.version, newestAny[e.name]->version) > 0)
            newestAny[e.name] = &e;

    vector<vector<string>> rows = {{"package", "latest", "description"}};
    for (auto &&[name, e] : newestAny)
    {
        if (!term.empty() && toLower(name).find(toLower(term)) == string::npos &&
            toLower(e->description).find(toLower(term)) == string::npos)
            continue;
        const PkgEntry *usable = newestFor(manifest, name, platform);
        rows.push_back({name, usable ? usable->version : "(none for this platform)", e->description});
    }
    if (rows.size() == 1)
    {
        cout << "No packages match '" << term << "'." << endl;
        return 0;
    }
    printTable(rows);
    cout << "Versions shown are the newest built for " << platform.full() << "." << endl;
    return 0;
}

// checkmake info <name>
int cmdInfo(const string &configPath, const string &name)
{
    BuildConfig config;
    Platform platform;
    vector<PkgEntry> manifest, locked;
    bool haveLock;
    if (!loadConfig(configPath, config) || !loadLock(config, locked, haveLock) || !detectPlatform(config, platform) ||
        !fetchManifest(config, manifest))
        return 1;

    vector<const PkgEntry *> builds;
    for (auto &&e : manifest)
        if (e.name == name)
            builds.push_back(&e);
    if (builds.empty())
    {
        cerr << "No package named '" << name << "' in the repo" << endl;
        return 1;
    }
    sort(builds.begin(), builds.end(), [](auto *a, auto *b)
         {
             int c = compareVersions(a->version, b->version);
             return c != 0 ? c > 0 : a->platform < b->platform;
         });

    cout << name << (builds[0]->description.empty() ? "" : " - " + builds[0]->description) << endl;
    for (auto &&r : config.packages)
        if (r.name == name)
            cout << "In " << configPath << ": " << r.text() << endl;
    cout << "Builds (newest first; this machine is " << platform.full() << "):" << endl;

    vector<vector<string>> rows;
    for (auto *b : builds)
    {
        string notes;
        if (platformMatch(b->platform, platform) == 0)
            notes = "not for this platform";
        for (auto &&l : locked)
            if (l.key() == b->key())
                notes = "in use";
        rows.push_back({b->version, b->platform, b->deps.empty() ? "no dependencies" : "needs " + formatDeps(b->deps),
                        notes.empty() ? "" : "(" + notes + ")"});
    }
    printTable(rows);
    return 0;
}
