// Signatures, hash pins, and the keygen/sign commands.
#include "checkmake.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Signatures
//
// Each dependency file F has a detached Ed25519 signature in F.sig, stored as
// hex. The signed message is the file's name (relative to the project root,
// or to the package root for packages), a NUL byte, then the file's
// contents. Including the name means a validly signed file can't be swapped
// in under another file's name.
//
// Uses libsodium's multi-part (Ed25519ph) API so large libs are streamed
// rather than loaded into memory.
// ---------------------------------------------------------------------------

// Stream a file into a signature state. If `hashOut` is given, also compute
// the file's BLAKE2b-256 hash in the same pass (used for hash pins), so
// pinned files are only read once.
static bool feedFile(crypto_sign_state &state, const string &path, const string &name, string *hashOut)
{
    ifstream file(path, ios::binary);
    if (!file.is_open())
        return false;

    crypto_sign_update(&state, reinterpret_cast<const unsigned char *>(name.c_str()), name.size() + 1);
    crypto_generichash_state hashState;
    if (hashOut)
        crypto_generichash_init(&hashState, nullptr, 0, HASH_BYTES);

    vector<char> buf(64 * 1024);
    while (file)
    {
        file.read(buf.data(), buf.size());
        if (file.gcount() > 0)
        {
            auto data = reinterpret_cast<const unsigned char *>(buf.data());
            crypto_sign_update(&state, data, file.gcount());
            if (hashOut)
                crypto_generichash_update(&hashState, data, file.gcount());
        }
    }
    if (hashOut)
    {
        unsigned char hash[HASH_BYTES];
        crypto_generichash_final(&hashState, hash, sizeof hash);
        *hashOut = binToHex(hash, sizeof hash);
    }
    return file.eof();
}

bool verifyFile(const string &path, const string &name, const unsigned char *pubkey, string &why, string *hashOut)
{
    string sigHex;
    if (!readToken(path + SIG_EXT, sigHex))
    {
        why = "missing signature file " + path + SIG_EXT;
        return false;
    }
    unsigned char sig[crypto_sign_BYTES];
    if (!hexToBin(sigHex, sig, sizeof sig))
    {
        why = "malformed signature in " + path + SIG_EXT;
        return false;
    }

    crypto_sign_state state;
    crypto_sign_init(&state);
    if (!feedFile(state, path, name, hashOut))
    {
        why = "can't read file";
        return false;
    }
    if (crypto_sign_final_verify(&state, sig, pubkey) != 0)
    {
        why = "BAD SIGNATURE";
        return false;
    }
    return true;
}

bool signFileAs(const string &path, const string &name, const unsigned char *secretKey)
{
    crypto_sign_state state;
    crypto_sign_init(&state);
    if (!feedFile(state, path, name, nullptr))
    {
        cerr << "Can't read '" << path << "'" << endl;
        return false;
    }
    unsigned char sig[crypto_sign_BYTES];
    crypto_sign_final_create(&state, sig, nullptr, secretKey);

    if (!writeFile(path + SIG_EXT, binToHex(sig, sizeof sig) + "\n"))
    {
        cerr << "Can't write '" << path << SIG_EXT << "'" << endl;
        return false;
    }
    cout << "signed " << name << endl;
    return true;
}

bool signTree(const fs::path &root, const fs::path &sub, const unsigned char *secretKey)
{
    bool ok = true;
    for (auto &entry : fs::recursive_directory_iterator(root / sub))
    {
        if (entry.is_symlink())
        {
            cerr << "Refusing to sign symlink " << entry.path().generic_string() << endl;
            ok = false;
        }
        else if (entry.is_regular_file() && entry.path().extension() != SIG_EXT)
        {
            string name = entry.path().lexically_relative(root).generic_string();
            ok &= signFileAs(entry.path().string(), name, secretKey);
        }
    }
    return ok;
}

// A dependency file: where it is on disk, and the name it was signed under.
struct DepFile
{
    string path;
    string signedName;
};

// Collect every file under root/sub, naming each relative to root. Symlinks
// and other non-regular files are rejected outright: they could point at
// unsigned files, and the compiler would follow them.
static void collectTree(const fs::path &root, const fs::path &sub, vector<DepFile> &files, int &bad)
{
    fs::path dir = root / sub;
    if (!fs::exists(dir))
        return;
    for (auto &entry : fs::recursive_directory_iterator(dir))
    {
        string shown = entry.path().generic_string();
        if (entry.is_symlink())
        {
            cerr << "  FAIL " << shown << ": symlinks aren't allowed" << endl;
            bad++;
        }
        else if (entry.is_directory())
            continue;
        else if (!entry.is_regular_file())
        {
            cerr << "  FAIL " << shown << ": not a regular file" << endl;
            bad++;
        }
        else if (entry.path().extension() != SIG_EXT)
            files.push_back({canonicalName(entry.path().string()),
                             entry.path().lexically_relative(root).generic_string()});
    }
}

int verifyTree(const fs::path &root, const fs::path &sub, const unsigned char *pubkey, int &bad)
{
    vector<DepFile> files;
    collectTree(root, sub, files, bad);
    int good = 0;
    for (auto &&f : files)
    {
        string why;
        if (verifyFile(f.path, f.signedName, pubkey, why))
            good++;
        else
        {
            cerr << "  FAIL " << f.path << ": " << why << endl;
            bad++;
        }
    }
    return good;
}

// Every local lib, every file under include/, and every file in every package.
static void collectDependencies(const BuildConfig &config, const Resolution &res, vector<DepFile> &files, int &bad)
{
    collectTree(".", INCLUDE_DIR, files, bad);
    for (auto &&lib : config.libs)
    {
        if (lib[0] == '-')
            cout << "  note: " << lib << " is a system library and isn't verified" << endl;
        else
            files.push_back({canonicalName(lib), canonicalName(lib)});
    }
    for (auto &&pkg : res)
        collectTree(pkg.entry.dir(), "", files, bad);
}

// ---------------------------------------------------------------------------
// Hash pins
//
// An optional pin file lists the exact BLAKE2b-256 hash of every dependency
// file, one "hash  path" per line (same layout as sha256sum). When the config
// sets `pins = <file>`, a build fails if any dependency's hash differs, if a
// dependency isn't pinned, or if a pinned file has gone missing -- even when
// everything is validly signed. `checkmake pin` writes the file.
// ---------------------------------------------------------------------------

static bool loadPins(const string &path, Pins &pins)
{
    ifstream file(path);
    if (!file.is_open())
    {
        cerr << "Can't open pin file '" << path << "' (run 'checkmake pin' to create it)" << endl;
        return false;
    }
    string line;
    int lineNum = 0;
    while (getline(file, line))
    {
        lineNum++;
        if (trim(line).empty() || trim(line)[0] == '#')
            continue;
        size_t space = line.find(' ');
        string hash = line.substr(0, space);
        string name = space == string::npos ? "" : trim(line.substr(space));
        unsigned char tmp[HASH_BYTES];
        if (name.empty() || !hexToBin(hash, tmp, sizeof tmp))
        {
            cerr << path << ":" << lineNum << ": expected 'hash  path'" << endl;
            return false;
        }
        pins[name] = hash;
    }
    return true;
}

bool writePins(const string &path, const Pins &pins)
{
    string text = "# checkmake hash pins: blake2b-256  path\n"
                  "# Regenerate with: checkmake pin <config>\n";
    for (auto &&[name, hash] : pins)
        text += hash + "  " + name + "\n";
    if (!writeFile(path, text))
    {
        cerr << "Can't write pin file '" << path << "'" << endl;
        return false;
    }
    return true;
}

bool verifyDependencies(const BuildConfig &config, const Resolution &res, Pins *hashesOut)
{
    cout << "Verifying dependencies against key " << binToHex(config.pubkey, sizeof config.pubkey) << endl;

    Pins pins;
    bool checkPins = !hashesOut && !config.pins.empty();
    if (checkPins)
    {
        if (!loadPins(config.pins, pins))
            return false;
        cout << "  and against hash pins in " << config.pins << endl;
    }

    int bad = 0;
    vector<DepFile> files;
    collectDependencies(config, res, files, bad);

    bool wantHash = checkPins || hashesOut;
    for (auto &&f : files)
    {
        string why, hash;
        if (!verifyFile(f.path, f.signedName, config.pubkey, why, wantHash ? &hash : nullptr))
        {
            cerr << "  FAIL " << f.path << ": " << why << endl;
            bad++;
            pins.erase(f.path); // it's present, just bad; don't also report it missing
            continue;
        }
        if (hashesOut)
            (*hashesOut)[f.path] = hash;
        if (checkPins)
        {
            auto it = pins.find(f.path);
            if (it == pins.end())
            {
                cerr << "  FAIL " << f.path << ": not in pin file" << endl;
                bad++;
            }
            else if (it->second != hash)
            {
                cerr << "  FAIL " << f.path << ": HASH MISMATCH (pinned " << it->second << ", got " << hash << ")"
                     << endl;
                bad++;
            }
            pins.erase(f.path);
        }
    }
    // Anything left in the pin list was expected but isn't there any more.
    for (auto &&[name, hash] : pins)
    {
        cerr << "  FAIL " << name << ": pinned but missing" << endl;
        bad++;
    }

    if (bad > 0)
    {
        cerr << "Dependency verification failed; not building." << endl;
        if (checkPins)
            cerr << "(If you changed dependencies on purpose, review them and run 'checkmake pin'.)" << endl;
        return false;
    }
    cout << "  " << files.size() << " file(s) OK" << endl;
    return true;
}

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

bool loadSecretKey(const string &keyPath, unsigned char *sk)
{
    string hex;
    bool ok = readToken(keyPath, hex) && hexToBin(hex, sk, crypto_sign_SECRETKEYBYTES);
    sodium_memzero(hex.data(), hex.size());
    if (!ok)
        cerr << "Can't read a secret key from '" << keyPath << "'" << endl;
    return ok;
}

// checkmake keygen <secret-key-file>
int cmdKeygen(const string &keyPath)
{
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);

    fs::path parent = fs::path(keyPath).parent_path();
    error_code ec;
    if (!parent.empty())
        fs::create_directories(parent, ec);

    // O_EXCL: never clobber an existing key. 0600: owner-only.
    int fd = open(keyPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        cerr << "Can't create '" << keyPath << "': " << strerror(errno) << endl;
        return 1;
    }
    string hex = binToHex(sk, sizeof sk) + "\n";
    sodium_memzero(sk, sizeof sk);
    bool written = write(fd, hex.data(), hex.size()) == (ssize_t)hex.size();
    sodium_memzero(hex.data(), hex.size());
    close(fd);
    if (!written)
    {
        cerr << "Failed writing '" << keyPath << "'" << endl;
        return 1;
    }

    cout << "Secret key written to " << keyPath << " (keep it private!)" << endl;
    cout << "Add this line to your config:" << endl;
    cout << "pubkey = " << binToHex(pk, sizeof pk) << endl;
    return 0;
}

// checkmake pubkey <secret-key-file>
//
// Prints just the public key's hex on stdout, so it can be used in scripts
// (e.g. --pubkey "$(checkmake pubkey key)").
int cmdPubkey(const string &keyPath)
{
    unsigned char sk[crypto_sign_SECRETKEYBYTES], pk[crypto_sign_PUBLICKEYBYTES];
    if (!loadSecretKey(keyPath, sk))
        return 1;
    crypto_sign_ed25519_sk_to_pk(pk, sk);
    sodium_memzero(sk, sizeof sk);
    cout << binToHex(pk, sizeof pk) << endl;
    return 0;
}

// checkmake sign <secret-key-file> <file-or-dir>...
int cmdSign(const string &keyPath, const vector<string> &targets)
{
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    if (!loadSecretKey(keyPath, sk))
        return 1;

    bool ok = true;
    for (auto &&t : targets)
    {
        if (fs::is_directory(t))
            ok &= signTree(".", fs::path(canonicalName(t)), sk);
        else
            ok &= signFileAs(t, canonicalName(t), sk);
    }
    sodium_memzero(sk, sizeof sk);
    return ok ? 0 : 1;
}
