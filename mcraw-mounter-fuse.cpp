#define FUSE_USE_VERSION 29

#include <fuse.h>
#include <vector>
#include <string>
#include <map>
#include <deque>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <cmath>
#include <filesystem>   // C++17 filesystem for cross-platform directory and path handling
#include <sys/stat.h>   // for mode constants
#include <cstring>      // for strerror
#include <cerrno>       // for errno

#include <motioncam/Decoder.hpp>
#include <audiofile/AudioFile.h>

#include "VirtualFileSystemImpl_MCRAW.h"

static std::map<std::string, std::unique_ptr<motioncam::VirtualFileSystemImpl_MCRAW>> contexts;

// report uniform size (0 until we have it)
static int fs_getattr(const char *path, struct stat *st)
{
    std::string p(path);
    memset(st, 0, sizeof(*st));

    if (p == "/") {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }

    // strip leading "/"
    auto slash = p.find('/', 1);
    if (slash == std::string::npos) {
        std::string base = p.substr(1);
        if (!contexts.count(base))
            return -ENOENT;
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }

    std::string base = p.substr(1, slash - 1);
    std::string name = p.substr(slash + 1);
    auto it = contexts.find(base);
    if (it == contexts.end())
        return -ENOENT;
    auto &ctx = it->second;

    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    if (name.ends_with(".wav")) {
        st->st_size = (off_t)ctx->getAudioSize();
    }
    else {
        st->st_size = (off_t)ctx->getTypicalDngSize();
    }

    return 0;
}

static int fs_readdir(const char *path, void *buf,
                      fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi)
{
    std::string p(path);
    (void)offset; (void)fi;

    if (p == "/") {
        filler(buf, ".", nullptr, 0);
        filler(buf, "..", nullptr, 0);
        for (auto &kv : contexts) {
            filler(buf, kv.first.c_str(), nullptr, 0);
        }
        return 0;
    }

    // strip leading "/"
    std::string rest = p.substr(1);
    // must be a context directory
    auto it = contexts.find(rest);
    if (it == contexts.end())
        return -ENOENT;
    auto &ctx = it->second;

    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);

    // list frames
    for (auto &f : ctx->listFiles())
        filler(buf, f.name.c_str(), nullptr, 0);

    std::string audioName = "audio.wav";
    filler(buf, audioName.c_str(), nullptr, 0);

    return 0;
}

static int fs_open(const char *path, struct fuse_file_info *fi)
{
    std::string p(path);
    // must be /<base>/<frame>
    if (p.size() < 2 || p[0] != '/')
        return -ENOENT;
    std::string rest = p.substr(1);
    auto slash = rest.find('/');
    if (slash == std::string::npos)
        return -EISDIR; // it's a directory, not a file

    std::string base = rest.substr(0, slash);
    std::string fname = rest.substr(slash + 1);
    auto it = contexts.find(base);
    if (it == contexts.end())
        return -ENOENT;
    auto &ctx = it->second;

    if (fname.ends_with("wav")) {
        return (fi->flags & 3) == O_RDONLY ? 0 : -EACCES;
    }

    // otherwise fall through to DNG frames
    if (!ctx->findEntry(fname).has_value())
        return -ENOENT;
    if ((fi->flags & 3) != O_RDONLY)
        return -EACCES;
    return 0;
}

static int fs_read(const char *path,
                   char *buf,
                   size_t size,
                   off_t offset,
                   struct fuse_file_info *fi)
{
    (void)fi;
    std::string p(path);
    // parse "/<base>/<frame>"
    std::string rest = p.substr(1);
    auto slash = rest.find('/');
    if (slash == std::string::npos)
        return -EISDIR;

    std::string base = rest.substr(0, slash);
    std::string fname = rest.substr(slash + 1);
    auto it = contexts.find(base);
    if (it == contexts.end())
        return -ENOENT;
    auto &ctx = it->second;
    auto entry = ctx->findEntry(fname);

    if (entry.has_value()) {
        return ctx->readFile(
            entry.value(),
            offset,
            size,
            buf,
            motioncam::EMPTY_CALLBACK,
            false
        );
    } else {
        return 0;
    }
}

static struct fuse_operations fs_ops = {
    .getattr = fs_getattr,
    .open    = fs_open,
    .read    = fs_read,
    .readdir = fs_readdir,
};

int main(int argc, char *argv[])
{
    // This program takes no arguments (we manage FUSE args ourselves).
    if (argc != 1) {
        std::cerr << "Usage: " << argv[0] << "\n";
        return 1;
    }

    namespace fs = std::filesystem;

    // 1) figure out our own executable's directory via argv[0]
    fs::path exePath = fs::weakly_canonical(argv[0]);
    std::string appDir = exePath.parent_path().string();

    // 2) scan that directory for *.mcraw files
    try {
        for (auto const& entry : fs::directory_iterator(appDir)) {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".mcraw")
                continue;
            std::string fn = entry.path().filename().string();
            std::string fullPath = entry.path().string();
            std::string baseName = entry.path().stem().string();

            std::cout << "Found file: " << fullPath << "\n";

            contexts.insert({baseName, std::make_unique<motioncam::VirtualFileSystemImpl_MCRAW>(fullPath)});
        }
    }
    catch (std::exception &e) {
        std::cerr << "Directory scan error: " << e.what() << "\n";
        return 1;
    }

    if (contexts.empty()) {
        std::cerr << "No .mcraw files found in " << appDir << "\n";
        return 1;
    }

    // 3) ensure the mount‐point exists
    std::string mountPoint = appDir + "/mcraws";
    try {
        if (!fs::exists(mountPoint))
            fs::create_directory(mountPoint);
        }
        catch (std::exception &e) {
        std::cerr << "Error creating mountpoint '" << mountPoint
        << "': " << e.what() << "\n";
        return 1;
    }

    // 4) assemble the same FUSE flags/options as original
    std::string volname = fs::path(mountPoint).filename().string();
    std::string mountOptions =
        "iosize=8388608,"
        "noappledouble,"
        "nobrowse,"
        "rdonly,"
        "noapplexattr,"
        "volname=" + volname;

    int fuse_argc = 6;
    char *fuse_argv[7];
    fuse_argv[0] = argv[0];
    fuse_argv[1] = (char*)"-f";  // foreground
    fuse_argv[2] = (char*)"-s";  // single-threaded
    fuse_argv[3] = (char*)"-o";  // mount options
    fuse_argv[4] = (char*)mountOptions.c_str();
    fuse_argv[5] = (char*)mountPoint.c_str();
    fuse_argv[6] = nullptr;

    // 5) run FUSE
    int ret = fuse_main(fuse_argc, fuse_argv, &fs_ops, nullptr);

    std::cout << "Exit code: " << ret;
    try {
        if (fs::exists(mountPoint))
            fs::remove(mountPoint);
    }
    catch (...) {
        std::cerr << "cleanup_mount: cannot remove '" << mountPoint << "'\n";
    }

    return ret;
}