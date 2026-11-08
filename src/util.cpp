// Small helpers: strings, hex, files, and running programs.
#include "checkmake.h"

#include <fstream>
#include <sstream>
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

string trim(const string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

vector<string> splitWords(const string &s)
{
    vector<string> words;
    istringstream in(s);
    string word;
    while (in >> word)
        words.push_back(word);
    return words;
}

vector<string> split(const string &s, char sep)
{
    vector<string> parts;
    string cur;
    for (char c : s)
    {
        if (c == sep)
        {
            parts.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    parts.push_back(cur);
    return parts;
}

string join(const vector<string> &parts, const string &sep)
{
    string s;
    for (size_t i = 0; i < parts.size(); i++)
        s += (i ? sep : "") + parts[i];
    return s;
}

bool isSafeName(const string &s)
{
    if (s.empty() || s[0] == '.' || s[0] == '-')
        return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '+' && c != '-')
            return false;
    return true;
}

bool hexToBin(const string &hex, unsigned char *out, size_t len)
{
    size_t binLen = 0;
    if (hex.size() != len * 2)
        return false;
    if (sodium_hex2bin(out, len, hex.c_str(), hex.size(), nullptr, &binLen, nullptr) != 0)
        return false;
    return binLen == len;
}

string binToHex(const unsigned char *bin, size_t len)
{
    string hex(len * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), bin, len);
    hex.pop_back();
    return hex;
}

bool readToken(const string &path, string &token)
{
    ifstream file(path);
    if (!file.is_open())
        return false;
    file >> token;
    return !token.empty();
}

string readFile(const string &path)
{
    ifstream file(path, ios::binary);
    stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool writeFile(const string &path, const string &content)
{
    ofstream out(path, ios::binary | ios::trunc);
    out << content;
    out.close();
    return out.good();
}

bool hashFile(const string &path, string &hexOut)
{
    ifstream file(path, ios::binary);
    if (!file.is_open())
        return false;
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, HASH_BYTES);
    vector<char> buf(64 * 1024);
    while (file)
    {
        file.read(buf.data(), buf.size());
        if (file.gcount() > 0)
            crypto_generichash_update(&state, reinterpret_cast<const unsigned char *>(buf.data()), file.gcount());
    }
    if (!file.eof())
        return false;
    unsigned char hash[HASH_BYTES];
    crypto_generichash_final(&state, hash, sizeof hash);
    hexOut = binToHex(hash, sizeof hash);
    return true;
}

string canonicalName(const string &path)
{
    fs::path p = fs::path(path).lexically_normal();
    if (p.is_absolute())
        p = p.lexically_relative(fs::current_path());
    return p.generic_string();
}

string siblingPath(const string &configPath, const string &ext)
{
    return fs::path(configPath).replace_extension(ext).generic_string();
}

string joinArgs(const vector<string> &args)
{
    return join(args, " ");
}

// Programs are run directly (no shell), so filenames with spaces or special
// characters can't break the command.
static vector<char *> makeArgv(const vector<string> &args)
{
    vector<char *> argv;
    for (auto &&a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    return argv;
}

pid_t spawnProcess(const vector<string> &args)
{
    vector<char *> argv = makeArgv(args);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
    {
        cerr << "Failed to run '" << args[0] << "'" << endl;
        return -1;
    }
    return pid;
}

int exitCode(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int waitFor(pid_t pid)
{
    int status;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return 1;
    return exitCode(status);
}

int runCommand(const vector<string> &args, bool echo)
{
    if (echo)
        cout << joinArgs(args) << endl;
    pid_t pid = spawnProcess(args);
    return pid < 0 ? 1 : waitFor(pid);
}

bool captureCommand(const vector<string> &args, string &out)
{
    int fds[2];
    if (pipe(fds) != 0)
        return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    vector<char *> argv = makeArgv(args);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0)
    {
        close(fds[0]);
        return false;
    }

    char buf[4096];
    while (true)
    {
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n > 0)
            out.append(buf, n);
        else if (n == 0 || errno != EINTR)
            break;
    }
    close(fds[0]);
    return waitFor(pid) == 0;
}

bool parseKeyValueFile(const string &path, vector<KeyValue> &out)
{
    ifstream file(path);
    if (!file.is_open())
    {
        cerr << "Can't open '" << path << "'" << endl;
        return false;
    }
    string line;
    int lineNum = 0;
    bool ok = true;
    while (getline(file, line))
    {
        lineNum++;
        size_t hash = line.find('#');
        if (hash != string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;
        size_t eq = line.find('=');
        if (eq == string::npos)
        {
            cerr << path << ":" << lineNum << ": expected 'key = value'" << endl;
            ok = false;
            continue;
        }
        out.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1)), lineNum});
    }
    return ok;
}

string toLower(string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

void printTable(const vector<vector<string>> &rows)
{
    vector<size_t> widths;
    for (auto &&r : rows)
        for (size_t i = 0; i < r.size(); i++)
        {
            if (widths.size() <= i)
                widths.push_back(0);
            widths[i] = max(widths[i], r[i].size());
        }
    for (auto &&r : rows)
    {
        string line;
        for (size_t i = 0; i < r.size(); i++)
            line += i + 1 < r.size() ? r[i] + string(widths[i] - r[i].size() + 2, ' ') : r[i];
        cout << "  " << trim(line) << endl;
    }
}
