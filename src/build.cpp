// Incremental, parallel builds.
//
// Each source compiles to its own object file under build/<config>/obj/.
// A source is recompiled only when:
//   - its object file is missing,
//   - its compile command changed (flags, compiler, include dirs, packages), or
//   - it or any header it includes is newer than the object.
// Header lists come from the compiler itself (-MMD -MP), so they're exact.
// The final link reruns only when an object or lib is newer than the output,
// or the link command changed.
//
// Dependencies are still signature-checked (and pin-checked) on every
// build, even when nothing needs compiling.
#include "checkmake.h"

#include <cerrno>
#include <cstdio>
#include <sys/wait.h>

struct CompileJob
{
    string source;
    string object;
    string depFile;
    string cmdFile;        // last successful compile command
    vector<string> args;   // full command, including dependency tracking
    vector<string> dbArgs; // same, minus dependency tracking, for editors
};

// One arg per line, so args containing spaces can't be confused.
static string serializeArgs(const vector<string> &args)
{
    string s;
    for (auto &&a : args)
        s += a + "\n";
    return s;
}

// Read the prerequisites of the first rule in a compiler-written .d file.
// Handles line continuations and escaped spaces.
static bool parseDepFile(const string &path, vector<string> &deps)
{
    string text = readFile(path);
    if (text.empty())
        return false;

    string tok;
    bool sawColon = false;
    auto flush = [&]()
    {
        if (tok.empty())
            return;
        if (!sawColon && tok.back() == ':')
            sawColon = true; // the target
        else if (sawColon)
            deps.push_back(tok);
        tok.clear();
    };

    for (size_t i = 0; i < text.size(); i++)
    {
        char c = text[i];
        char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '\\' && next == '\n')
        {
            flush();
            i++;
        }
        else if (c == '\\' && next == ' ')
        {
            tok += ' ';
            i++;
        }
        else if (c == '$' && next == '$')
        {
            tok += '$';
            i++;
        }
        else if (c == '\n')
        {
            flush();
            if (sawColon)
                break; // end of the first rule
        }
        else if (c == ' ' || c == '\t' || c == '\r')
            flush();
        else
            tok += c;
    }
    flush();
    return sawColon;
}

// True if `path` is missing or newer than `than`.
static bool newerThan(const string &path, fs::file_time_type than)
{
    error_code ec;
    auto t = fs::last_write_time(path, ec);
    return ec || t > than;
}

static bool needsCompile(const CompileJob &job)
{
    error_code ec;
    auto objTime = fs::last_write_time(job.object, ec);
    if (ec)
        return true;
    if (readFile(job.cmdFile) != serializeArgs(job.args))
        return true;
    vector<string> deps;
    if (!parseDepFile(job.depFile, deps))
        return true;
    for (auto &&d : deps)
        if (newerThan(d, objTime))
            return true;
    return false;
}

// Run compile jobs, at most opts.jobs at a time. Stops starting new jobs
// after the first failure, but lets running ones finish.
static bool runJobs(vector<CompileJob *> &jobs, const BuildOptions &opts)
{
    map<pid_t, CompileJob *> running;
    size_t next = 0;
    bool failed = false;

    while (!running.empty() || (!failed && next < jobs.size()))
    {
        while (!failed && next < jobs.size() && running.size() < opts.jobs)
        {
            CompileJob &job = *jobs[next++];
            cout << "[" << next << "/" << jobs.size() << "] "
                 << (opts.verbose ? joinArgs(job.args) : "compile " + job.source) << endl;
            fs::create_directories(fs::path(job.object).parent_path());
            // Forget the old command until this compile succeeds, so an
            // interrupted or failed compile is always retried.
            fs::remove(job.cmdFile);
            pid_t pid = spawnProcess(job.args);
            if (pid < 0)
                failed = true;
            else
                running[pid] = &job;
        }
        if (running.empty())
            break;

        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        auto it = running.find(pid);
        if (it == running.end())
            continue;
        CompileJob &job = *it->second;
        running.erase(it);

        if (exitCode(status) == 0)
            writeFile(job.cmdFile, serializeArgs(job.args));
        else
        {
            cerr << "Failed to compile " << job.source << endl;
            failed = true;
        }
    }
    return !failed;
}

static string jsonString(const string &s)
{
    string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
            out += string("\\") + (char)c;
        else if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c < 0x20)
        {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        }
        else
            out += (char)c;
    }
    return out + "\"";
}

// compile_commands.json, for clangd and other editor tooling. Only rewritten
// when it changes, so editors don't reload it for nothing.
static void writeCompileDb(const vector<CompileJob> &jobs)
{
    string dir = fs::current_path().string();
    string json = "[\n";
    for (size_t i = 0; i < jobs.size(); i++)
    {
        json += "  {\n    \"directory\": " + jsonString(dir) + ",\n";
        json += "    \"file\": " + jsonString(jobs[i].source) + ",\n";
        json += "    \"output\": " + jsonString(jobs[i].object) + ",\n";
        json += "    \"arguments\": [";
        for (size_t a = 0; a < jobs[i].dbArgs.size(); a++)
            json += (a ? ", " : "") + jsonString(jobs[i].dbArgs[a]);
        json += "]\n  }" + string(i + 1 < jobs.size() ? "," : "") + "\n";
    }
    json += "]\n";
    if (readFile(COMPILE_DB) != json)
        writeFile(COMPILE_DB, json);
}

// Copy each package's (already verified) license.txt to
// licenses/<name>-<version>.txt, so the project carries the licenses of
// everything it links. Only rewritten when the contents change.
static void collectLicenses(const Resolution &res)
{
    for (auto &&pkg : res)
    {
        fs::path src = fs::path(pkg.entry.dir()) / LICENSE_FILE;
        if (!fs::is_regular_file(src))
        {
            cout << "  note: " << pkg.entry.id() << " has no " << LICENSE_FILE << endl;
            continue;
        }
        fs::create_directories(LICENSES_DIR);
        string dest = LICENSES_DIR + "/" + pkg.entry.name + "-" + pkg.entry.version + ".txt";
        string text = readFile(src.string());
        if (readFile(dest) != text)
        {
            writeFile(dest, text);
            cout << "  license for " << pkg.entry.id() << " -> " << dest << endl;
        }
    }
}

// Per-config build folder, so two configs in one project don't fight.
static string buildDirFor(const string &configPath)
{
    return BUILD_DIR + "/" + fs::path(configPath).filename().string();
}

int cmdBuild(const string &configPath, const BuildOptions &opts)
{
    BuildConfig config;
    if (!loadConfig(configPath, config))
        return 1;
    Resolution res;
    if (!syncPackages(config, res))
        return 1;
    if (!verifyDependencies(config, res))
        return 1;
    collectLicenses(res);

    // Shared compile flags: local includes, package includes and defines, then the config's flags.
    vector<string> base = {config.compiler};
    if (fs::is_directory(INCLUDE_DIR))
        base.insert(base.end(), {"-I", INCLUDE_DIR});
    for (auto &&pkg : res)
        if (fs::is_directory(fs::path(pkg.entry.dir()) / INCLUDE_DIR))
            base.insert(base.end(), {"-I", pkg.entry.dir() + "/" + INCLUDE_DIR});
    for (auto &&pkg : res)
        for (auto &&d : pkg.meta.defines)
            base.push_back("-D" + d);
    base.insert(base.end(), config.flags.begin(), config.flags.end());

    string objDir = buildDirFor(configPath) + "/obj";
    vector<CompileJob> jobs;
    for (auto &&src : config.sources)
    {
        string norm = fs::path(src).lexically_normal().generic_string();
        if (norm.rfind("..", 0) == 0)
        {
            cerr << "Source '" << src << "' is outside the project" << endl;
            return 1;
        }
        CompileJob job;
        job.source = norm;
        job.object = objDir + "/" + norm + ".o";
        job.depFile = job.object + ".d";
        job.cmdFile = job.object + ".cmd";
        job.dbArgs = base;
        job.dbArgs.insert(job.dbArgs.end(), {"-c", norm, "-o", job.object});
        job.args = base;
        job.args.insert(job.args.end(), {"-MMD", "-MP", "-MF", job.depFile, "-c", norm, "-o", job.object});
        jobs.push_back(job);
    }
    writeCompileDb(jobs);

    vector<CompileJob *> stale;
    for (auto &job : jobs)
        if (needsCompile(job))
            stale.push_back(&job);

    if (!stale.empty())
    {
        cout << "Compiling " << stale.size() << " of " << jobs.size() << " source(s) with "
             << min<size_t>(opts.jobs, stale.size()) << " job(s)" << endl;
        if (!runJobs(stale, opts))
        {
            cerr << "Build failed" << endl;
            return 1;
        }
    }

    // Link: objects, local libs, package libs (dependents before their
    // dependencies, as static linking needs), then system libraries.
    vector<string> link = {config.compiler};
    link.insert(link.end(), config.flags.begin(), config.flags.end());
    vector<string> linkInputs;
    for (auto &&job : jobs)
        linkInputs.push_back(job.object);
    for (auto &&lib : config.libs)
        linkInputs.push_back(lib);
    for (auto &&pkg : res)
        for (auto &&lib : packageLibFiles(pkg))
            linkInputs.push_back(lib);
    set<string> seenSys;
    for (auto &&pkg : res)
        for (auto &&s : pkg.meta.syslibs)
            if (seenSys.insert(s).second)
                linkInputs.push_back("-l" + s);
    link.insert(link.end(), linkInputs.begin(), linkInputs.end());
    link.insert(link.end(), {"-o", config.output});

    string linkCmdFile = buildDirFor(configPath) + "/link.cmd";
    bool relink = !stale.empty() || readFile(linkCmdFile) != serializeArgs(link);
    error_code ec;
    auto outTime = fs::last_write_time(config.output, ec);
    if (ec)
        relink = true;
    for (size_t i = 0; !relink && i < linkInputs.size(); i++)
        if (linkInputs[i][0] != '-' && newerThan(linkInputs[i], outTime))
            relink = true;

    if (!relink)
    {
        cout << config.output << " is up to date" << endl;
        return 0;
    }

    fs::create_directories(buildDirFor(configPath));
    fs::remove(linkCmdFile);
    cout << (opts.verbose ? joinArgs(link) : "link " + config.output) << endl;
    if (runCommand(link, false) != 0)
    {
        cerr << "Link failed" << endl;
        return 1;
    }
    writeFile(linkCmdFile, serializeArgs(link));
    cout << "Built " << config.output << endl;
    return 0;
}

static void removeAndReport(const string &path)
{
    error_code ec;
    if (fs::exists(path, ec))
    {
        fs::remove_all(path);
        cout << "removed " << path << endl;
    }
}

// checkmake clean [config]: delete this config's objects and output.
int cmdClean(const string &configPath)
{
    BuildConfig config;
    if (!loadConfig(configPath, config))
        return 1;
    removeAndReport(buildDirFor(configPath));
    removeAndReport(config.output);
    removeAndReport(COMPILE_DB);
    // Drop build/ itself once the last config's folder is gone.
    error_code ec;
    if (fs::is_empty(BUILD_DIR, ec) && !ec)
        fs::remove(BUILD_DIR);
    return 0;
}
