// CheckMake entry point: command-line parsing.
#include "checkmake.h"

#include <thread>

static void usage(const char *prog)
{
    string p = fs::path(prog).filename().string();
    cerr << "Usage: " << p << " [command] [options]\n"
         << "\n"
         << "Building\n"
         << "  " << p << " [-j N] [-v] [config]      build incrementally (default config: build.cm)\n"
         << "                                  -j N: parallel compile jobs (default: CPU count)\n"
         << "                                  -v:   show full compiler commands\n"
         << "  " << p << " clean [config]            delete objects, output and compile_commands.json\n"
         << "\n"
         << "Packages (add -c <config> to use a config other than build.cm)\n"
         << "  " << p << " add <name> [range]        add or change a package, e.g. add zlib ^1.3\n"
         << "  " << p << " remove <name>...          remove packages\n"
         << "  " << p << " update [name...]          move to the newest versions the config allows\n"
         << "  " << p << " outdated                  show packages with newer versions available\n"
         << "  " << p << " list                      show the installed packages as a tree\n"
         << "  " << p << " search [term]             search the repo\n"
         << "  " << p << " info <name>               show a package's versions and dependencies\n"
         << "  " << p << " prune [config...]         delete deps/ packages none of the configs use\n"
         << "  " << p << " pin [config]              pin current dependency hashes and enable pinning\n"
         << "\n"
         << "Signing and publishing\n"
         << "  " << p << " keygen <secret-key-file>  make a signing keypair\n"
         << "  " << p << " pubkey <secret-key-file>  print the public key (hex) for a secret key file\n"
         << "  " << p << " sign <secret-key-file> <file-or-dir>...\n"
         << "                                  write .sig files for local libs/includes\n"
         << "  " << p << " pack <secret-key-file> <package-dir> <repo-dir>\n"
         << "                                  sign a package (see package.cm) and publish it to a repo\n"
         << "\n"
         << "Repo folders\n"
         << "  " << p << " repo init <key> <dir>     create an empty, signed repo\n"
         << "  " << p << " repo list <dir>           list every build in a repo (--pubkey HEX to check it)\n"
         << "  " << p << " repo search <dir> <term>  find builds by name or description\n"
         << "  " << p << " repo remove <key> <dir> <name>[@version] [--platform P] [-y]\n"
         << "                                  remove builds from a repo and re-sign its manifest\n"
         << "\n"
         << "Version ranges: ^1.2 (>=1.2 <2), ~1.2 (>=1.2 <1.3), >=1.2 <2, =1.2, 1.2, *\n";
}

// Pull "-c <config>" / "--config <config>" out of the args.
static bool takeConfigOption(vector<string> &args, string &config)
{
    for (size_t i = 0; i < args.size(); i++)
        if (args[i] == "-c" || args[i] == "--config")
        {
            if (i + 1 >= args.size())
                return false;
            config = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            return true;
        }
    return true;
}

static int run(vector<string> args, const char *prog)
{
    string cmd = args.empty() ? "" : args[0];
    vector<string> rest = args.empty() ? args : vector<string>(args.begin() + 1, args.end());
    string config = DEFAULT_CONFIG;

    auto bad = [&]()
    {
        usage(prog);
        return 1;
    };

    if (cmd == "-h" || cmd == "--help" || cmd == "help")
    {
        usage(prog);
        return 0;
    }
    if (cmd == "keygen")
        return rest.size() == 1 ? cmdKeygen(rest[0]) : bad();
    if (cmd == "pubkey")
        return rest.size() == 1 ? cmdPubkey(rest[0]) : bad();
    if (cmd == "sign")
        return rest.size() >= 2 ? cmdSign(rest[0], vector<string>(rest.begin() + 1, rest.end())) : bad();
    if (cmd == "pack")
        return rest.size() == 3 ? cmdPack(rest[0], rest[1], rest[2]) : bad();
    if (cmd == "repo")
        return cmdRepo(rest);

    // Everything below works on a config.
    static const set<string> configCommands = {"pin",    "clean",    "prune", "add",    "remove",
                                               "update", "outdated", "list",  "search", "info"};
    if (configCommands.count(cmd))
    {
        if (!takeConfigOption(rest, config))
            return bad();
        if (cmd == "pin" || cmd == "clean")
        {
            if (rest.size() > 1)
                return bad();
            if (rest.size() == 1)
                config = rest[0];
            return cmd == "pin" ? cmdPin(config) : cmdClean(config);
        }
        if (cmd == "prune")
            return cmdPrune(rest.empty() ? vector<string>{config} : rest);
        if (cmd == "add")
            return rest.empty() ? bad() : cmdAdd(config, rest);
        if (cmd == "remove")
            return rest.empty() ? bad() : cmdRemove(config, rest);
        if (cmd == "update")
            return cmdUpdate(config, rest);
        if (cmd == "outdated")
            return rest.empty() ? cmdOutdated(config) : bad();
        if (cmd == "list")
            return rest.empty() ? cmdList(config) : bad();
        if (cmd == "search")
            return rest.size() <= 1 ? cmdSearch(config, rest.empty() ? "" : rest[0]) : bad();
        if (cmd == "info")
            return rest.size() == 1 ? cmdInfo(config, rest[0]) : bad();
    }

    // Build: [build] [-j N | -jN] [-v] [config]
    if (cmd == "build")
        args = rest;
    BuildOptions opts;
    opts.jobs = max(1u, thread::hardware_concurrency());
    bool haveConfig = false;
    for (size_t i = 0; i < args.size(); i++)
    {
        const string &a = args[i];
        string jobsStr;
        if (a == "-j" && i + 1 < args.size())
            jobsStr = args[++i];
        else if (a.rfind("-j", 0) == 0 && a.size() > 2)
            jobsStr = a.substr(2);
        else if (a == "-v")
        {
            opts.verbose = true;
            continue;
        }
        else if ((a == "-c" || a == "--config") && i + 1 < args.size() && !haveConfig)
        {
            config = args[++i];
            haveConfig = true;
            continue;
        }
        else if (!a.empty() && a[0] != '-' && !haveConfig)
        {
            if (!fs::exists(a))
            {
                cerr << "Unknown command or config file '" << a << "'" << endl;
                return bad();
            }
            config = a;
            haveConfig = true;
            continue;
        }
        else
            return bad();

        try
        {
            int n = stoi(jobsStr);
            if (n < 1)
                throw invalid_argument("");
            opts.jobs = n;
        }
        catch (const exception &)
        {
            cerr << "-j needs a positive number" << endl;
            return 1;
        }
    }
    return cmdBuild(config, opts);
}

int main(int argc, char **argv)
{
    if (sodium_init() < 0)
    {
        // Doubleplus ungood! The crypto has failed!
        cerr << "CRYPTO ERROR: Failed to init." << endl;
        return 1;
    }

    try
    {
        return run(vector<string>(argv + 1, argv + argc), argv[0]);
    }
    catch (const fs::filesystem_error &e)
    {
        cerr << "Filesystem error: " << e.what() << endl;
        return 1;
    }
}
