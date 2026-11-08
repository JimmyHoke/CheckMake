// CheckMake: a signed, incremental C++ build system with a package manager.
// Shared declarations for every module.
#pragma once

#include <sodium.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sys/types.h>

using namespace std;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Names of files and folders
// ---------------------------------------------------------------------------

inline const string DEFAULT_CONFIG = "build.cm";
inline const string INCLUDE_DIR = "include";
inline const string LIB_DIR = "lib";
inline const string DEPS_DIR = "deps";
inline const string DOWNLOAD_DIR = "deps/.download";
inline const string BUILD_DIR = "build";
inline const string COMPILE_DB = "compile_commands.json";
inline const string LICENSES_DIR = "licenses";   // where builds collect package licenses
inline const string SIG_EXT = ".sig";
inline const string MANIFEST = "manifest";
inline const string PKG_EXT = ".tar.xz";
inline const string PACKAGE_META = "package.cm";  // metadata at a package's root
inline const string LICENSE_FILE = "license.txt"; // optional, at a package's root

inline constexpr size_t HASH_BYTES = crypto_generichash_BYTES; // BLAKE2b-256

// ---------------------------------------------------------------------------
// util.cpp
// ---------------------------------------------------------------------------

string trim(const string &s);
vector<string> splitWords(const string &s);
vector<string> split(const string &s, char sep);
string join(const vector<string> &parts, const string &sep);
// Letters, digits and . _ + - only, not starting with . or -. Used for
// anything that ends up in a file path.
bool isSafeName(const string &s);
bool hexToBin(const string &hex, unsigned char *out, size_t len);
string binToHex(const unsigned char *bin, size_t len);
bool readToken(const string &path, string &token);
string readFile(const string &path); // "" if missing
bool writeFile(const string &path, const string &content);
bool hashFile(const string &path, string &hexOut); // BLAKE2b-256
// Normalize a path so "./lib/x.a" and "lib/x.a" are the same name.
string canonicalName(const string &path);
// build.cm -> build.lock etc.
string siblingPath(const string &configPath, const string &ext);

string toLower(string s);
// Print rows as aligned columns (the last column isn't padded).
void printTable(const vector<vector<string>> &rows);

string joinArgs(const vector<string> &args);
pid_t spawnProcess(const vector<string> &args); // -1 on failure
int exitCode(int status);
int runCommand(const vector<string> &args, bool echo = true);
bool captureCommand(const vector<string> &args, string &out); // stdout; true on exit 0

// "key = value" files with # comments (configs and package.cm).
struct KeyValue
{
    string key;
    string value;
    int line;
};
bool parseKeyValueFile(const string &path, vector<KeyValue> &out);

// ---------------------------------------------------------------------------
// versions.cpp: versions, requirements and platforms
// ---------------------------------------------------------------------------

// Compare versions segment by segment: "1.10" > "1.9", "1.0.22-2" > "1.0.22".
int compareVersions(const string &a, const string &b);

struct Constraint
{
    string op; // = > >= < <= *
    string version;
};

// "zlib >=1.2 <2", "fmt ^11.0", "sodium@1.0.22" (exact), "hdr" (any)
struct Requirement
{
    string name;
    string spec; // the constraint text as written, e.g. "^1.2"; "" = any
    vector<Constraint> constraints;
    string text() const { return spec.empty() ? name : name + " " + spec; }
};
bool parseRequirement(const string &text, Requirement &req, string &err);
bool satisfies(const string &version, const Requirement &req);

// A build target, e.g. x86_64-linux-gnu-libstdc++.
struct Platform
{
    string arch, os, libc, cxx;
    string base() const { return arch + "-" + os + "-" + libc; } // C ABI
    string full() const { return base() + "-" + cxx; }            // C++ ABI
};
bool isValidPackagePlatform(const string &p); // "any", base or full form
bool parsePlatform(const string &p, Platform &out); // full form only
// How well a package's platform fits: 0 = not usable, 1 = any, 2 = C ABI, 3 = exact.
int platformMatch(const string &pkgPlatform, const Platform &target);

// ---------------------------------------------------------------------------
// config.cpp
// ---------------------------------------------------------------------------

struct BuildConfig
{
    string path;
    string compiler = "g++";
    string output = "programout";
    string repo;
    string pins;
    string platform; // override; detected from the compiler if empty
    vector<string> sources;
    vector<string> libs;
    vector<string> flags;
    vector<Requirement> packages;
    bool hasPubkey = false;
    unsigned char pubkey[crypto_sign_PUBLICKEYBYTES];
};
bool loadConfig(const string &path, BuildConfig &config);
bool detectPlatform(const BuildConfig &config, Platform &out);
string lockPathFor(const string &configPath);
// Remove `name` from a config's package lines; returns the new text.
string removePackageFromConfigText(const string &text, const string &name, bool &removed);

// ---------------------------------------------------------------------------
// repo.cpp: package entries, metadata, resolution, installation
// ---------------------------------------------------------------------------

// One published package build: a manifest or lock file line.
struct PkgEntry
{
    string name, version, platform, hash, description;
    vector<Requirement> deps;
    string id() const { return name + "@" + version; }
    string key() const { return id() + "/" + platform; }
    string fileName() const { return name + "-" + version + "-" + platform + PKG_EXT; }
    string dir() const { return DEPS_DIR + "/" + name + "-" + version + "-" + platform; }
};
bool loadEntries(const string &path, vector<PkgEntry> &entries, const string &what);
string formatDeps(const vector<Requirement> &deps);

// package.cm
struct PackageMeta
{
    string name, version, platform, description;
    vector<Requirement> deps;
    vector<string> libs;    // in link order, relative to lib/
    vector<string> defines; // -D for anything that includes the package
    vector<string> syslibs; // system libraries, e.g. pthread -> -lpthread
};
bool loadPackageMeta(const string &path, PackageMeta &meta);

struct ResolvedPackage
{
    PkgEntry entry;
    PackageMeta meta;
    bool direct = false;
};
using Resolution = vector<ResolvedPackage>; // link order: dependents first

vector<string> packageLibFiles(const ResolvedPackage &pkg);

bool fetchManifest(const BuildConfig &config, vector<PkgEntry> &entries);
// Solve `config.packages` from `candidates`, preferring the versions in `prefer`.
bool resolvePackages(const BuildConfig &config, const Platform &platform, const vector<PkgEntry> &candidates,
                     const map<string, string> &prefer, map<string, PkgEntry> &chosen, string &err);

enum class SyncMode
{
    Normal, // keep locked versions whenever they still fit
    Update  // pick the newest allowed versions (of `names`, or of everything)
};
// Resolve, update the lock file, download anything missing, and load
// verified metadata. The heart of build/add/remove/update.
bool syncPackages(const BuildConfig &config, Resolution &out, SyncMode mode = SyncMode::Normal,
                  const set<string> &names = {});

// A repo folder on this machine. With `pubkey`, the manifest's signature is
// checked; a missing manifest gives an empty list and exists = false.
bool loadLocalManifest(const string &repoDir, const unsigned char *pubkey, vector<PkgEntry> &entries, bool &exists);
bool writeLocalManifest(const string &repoDir, const vector<PkgEntry> &entries, const unsigned char *secretKey);

int cmdPack(const string &keyPath, const string &pkgDir, const string &repoDir);

// ---------------------------------------------------------------------------
// repoadmin.cpp: checkmake repo init / list / search / remove
// ---------------------------------------------------------------------------

int cmdRepo(const vector<string> &args);

// ---------------------------------------------------------------------------
// signing.cpp: signatures and hash pins
// ---------------------------------------------------------------------------

bool verifyFile(const string &path, const string &name, const unsigned char *pubkey, string &why,
                string *hashOut = nullptr);
bool signFileAs(const string &path, const string &name, const unsigned char *secretKey);
bool signTree(const fs::path &root, const fs::path &sub, const unsigned char *secretKey);
int verifyTree(const fs::path &root, const fs::path &sub, const unsigned char *pubkey, int &bad);
bool loadSecretKey(const string &keyPath, unsigned char *sk);

using Pins = map<string, string>; // path -> hash hex
bool writePins(const string &path, const Pins &pins);
// Signature-check every dependency, and hash-check it if pins are on. With
// `hashesOut`, collects the hashes instead of consulting the pin file.
bool verifyDependencies(const BuildConfig &config, const Resolution &res, Pins *hashesOut = nullptr);

int cmdKeygen(const string &keyPath);
int cmdPubkey(const string &keyPath);
int cmdSign(const string &keyPath, const vector<string> &targets);

// ---------------------------------------------------------------------------
// build.cpp
// ---------------------------------------------------------------------------

struct BuildOptions
{
    unsigned jobs = 1;
    bool verbose = false;
};
int cmdBuild(const string &configPath, const BuildOptions &opts);
int cmdClean(const string &configPath);

// ---------------------------------------------------------------------------
// commands.cpp: package management commands
// ---------------------------------------------------------------------------

int cmdPin(const string &configPath);
int cmdPrune(const vector<string> &configPaths);
int cmdAdd(const string &configPath, const vector<string> &args);
int cmdRemove(const string &configPath, const vector<string> &names);
int cmdUpdate(const string &configPath, const vector<string> &names);
int cmdOutdated(const string &configPath);
int cmdList(const string &configPath);
int cmdSearch(const string &configPath, const string &term);
int cmdInfo(const string &configPath, const string &name);
