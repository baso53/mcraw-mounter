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

#define TINY_DNG_WRITER_IMPLEMENTATION
    #include <tinydng/tiny_dng_writer.h>
#undef TINY_DNG_WRITER_IMPLEMENTATION

bool getAudio(
    std::vector<uint8_t>& fileData,
    const int sampleRateHz,
    const int numChannels,
    std::vector<motioncam::AudioChunk>& audioChunks)
{
    AudioFile<int16_t> audio;
    
    audio.setNumChannels(numChannels);
    audio.setSampleRate(sampleRateHz);
    
    if(numChannels == 2) {
        for(auto& x : audioChunks) {
            for(auto i = 0; i < x.second.size(); i+=2) {
                audio.samples[0].push_back(x.second[i]);
                audio.samples[1].push_back(x.second[i+1]);
            }
        }
    }
    else if(numChannels == 1) {
        for(auto& x : audioChunks) {
            for(auto i = 0; i < x.second.size(); i++)
                audio.samples[0].push_back(x.second[i]);
        }
    }

    return audio.getFileData(fileData);
}

struct FSContext {
    motioncam::Decoder *decoder = nullptr;
    std::map<std::string, motioncam::Timestamp> dngFileNameToFrameTimestamp;
    std::map<std::string, std::vector<char>> dngFileNameToFrameCache;
    std::deque<std::string> frameCacheOrder;
    static constexpr size_t MAX_CACHE_FRAMES = 3;
    size_t frameSize = 0;

    std::vector<uint16_t> blackLevels;
    double whiteLevel = 0.0;
    std::array<uint8_t, 4> cfa = {{0, 1, 1, 2}};
    uint16_t orientation;
    std::vector<float> colorMatrix1,
        colorMatrix2,
        forwardMatrix1,
        forwardMatrix2;
    std::vector<uint8_t> audioWavData;
    size_t               audioSize = 0;

    std::string mcrawBaseName;
};

static std::map<std::string, FSContext> contexts;

// call this once, right after containerMetadata is set:
static void cache_container_metadata(FSContext *ctx, nlohmann::json *containerMetadata)
{
    // Black levels
    std::vector<uint16_t> blackLevel = (*containerMetadata)["blackLevel"];
    ctx->blackLevels.reserve(blackLevel.size());
    for (float v : blackLevel)
        ctx->blackLevels.push_back(uint16_t(std::lround(v)));

    // White level
    ctx->whiteLevel = (*containerMetadata)["whiteLevel"];

    // CFA pattern
    std::string sensorArrangement = (*containerMetadata)["sensorArrangment"];
    ctx->colorMatrix1 = (*containerMetadata)["colorMatrix1"].get<std::vector<float>>();
    ctx->colorMatrix2 = (*containerMetadata)["colorMatrix2"].get<std::vector<float>>();
    ctx->forwardMatrix1 = (*containerMetadata)["forwardMatrix1"].get<std::vector<float>>();
    ctx->forwardMatrix2 = (*containerMetadata)["forwardMatrix2"].get<std::vector<float>>();

    if (sensorArrangement == "rggb")
        ctx->cfa = {{0, 1, 1, 2}};
    else if (sensorArrangement == "bggr")
        ctx->cfa = {{2, 1, 1, 0}};
    else if (sensorArrangement == "grbg")
        ctx->cfa = {{1, 0, 2, 1}};
    else if (sensorArrangement == "gbrg")
        ctx->cfa = {{1, 2, 0, 1}};
    else
        ctx->cfa = {{0, 1, 1, 2}};
}

static std::string frameName(const std::string &base, int i)
{
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s_%06d.dng", base.c_str(), i);
    return buf;
}

// decode one frame into frameCache[path]
// after writing to cache, if this is the first frame, record its size
static int load_frame(FSContext *ctx, const std::string &path, size_t* size)
{
    // fast‐path if cached
    if (ctx->dngFileNameToFrameCache.count(path))
        return 0;

    motioncam::Timestamp timestamp = ctx->dngFileNameToFrameTimestamp.at(path);

    // decode raw + per‐frame metadata
    std::vector<uint8_t> raw;
    nlohmann::json metadata;
    try
    {
        ctx->decoder->loadFrame(timestamp, raw, metadata);
    }
    catch (std::exception &e)
    {
        std::cerr << "EIO error: " << e.what() << "\n";
        return -EIO;
    }

    // pack into a DNGImage
    tinydngwriter::DNGImage dng;
    dng.SetCustomFieldLong(0x23, 23);
    const unsigned int width = metadata["width"];
    const unsigned int height = metadata["height"];
    std::vector<float> asShotNeutral = metadata["asShotNeutral"];
    dng.SetBigEndian(false);
    dng.SetDNGVersion(1, 4, 0, 0);
    dng.SetDNGBackwardVersion(1, 1, 0, 0);
    dng.SetImageData(
        (const unsigned char *)raw.data(),
        raw.size());
    dng.SetImageWidth(width);
    dng.SetImageLength(height);
    dng.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA);
    dng.SetRowsPerStrip(height);
    dng.SetSamplesPerPixel(1);
    dng.SetCFARepeatPatternDim(2, 2);
    
    dng.SetBlackLevelRepeatDim(2, 2);
    dng.SetBlackLevel(uint32_t(ctx->blackLevels.size()), ctx->blackLevels.data());
    dng.SetWhiteLevel(ctx->whiteLevel);
    dng.SetCompression(tinydngwriter::COMPRESSION_NONE);

    dng.SetCFAPattern(4, ctx->cfa.data());
    
    // Rectangular
    dng.SetCFALayout(1);

    const uint16_t bps[1] = { 16 };
    dng.SetBitsPerSample(1, bps);
    
    dng.SetColorMatrix1(3, ctx->colorMatrix1.data());
    dng.SetColorMatrix2(3, ctx->colorMatrix2.data());

    dng.SetForwardMatrix1(3, ctx->forwardMatrix1.data());
    dng.SetForwardMatrix2(3, ctx->forwardMatrix2.data());
    
    dng.SetAsShotNeutral(3, asShotNeutral.data());
    
    dng.SetCalibrationIlluminant1(21);
    dng.SetCalibrationIlluminant2(17);
    
    dng.SetUniqueCameraModel("MotionCam");
    dng.SetSubfileType();
    
    const uint32_t activeArea[4] = { 0, 0, height, width };
    dng.SetActiveArea(&activeArea[0]);
    if (ctx->orientation) {
        dng.SetOrientation(ctx->orientation);
    }

    // Write DNG
    std::string err;
    tinydngwriter::DNGWriter writer(false);
    writer.AddImage(&dng);
    
    std::vector<char> outputBuffer;
    boost::vectorbuf outputBufferVectorBuf(outputBuffer);
    std::ostream outputStream(&outputBufferVectorBuf);

    if (!writer.WriteToFile(outputStream, &err))
    {
        std::cerr << "DNG pack error: " << err << "\n";
        return -EIO;
    }

    // insert into rolling‐buffer cache
    if (ctx->dngFileNameToFrameCache.size() >= FSContext::MAX_CACHE_FRAMES)
    {
        ctx->dngFileNameToFrameCache.erase(ctx->frameCacheOrder.front());
        ctx->frameCacheOrder.pop_front();
    }
    ctx->dngFileNameToFrameCache[path] = std::move(outputBuffer);
    ctx->frameCacheOrder.push_back(path);

    (*size) = ctx->dngFileNameToFrameCache[path].size();

    return 0;
}

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
    FSContext &ctx = it->second;

    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    if (name == "audio.wav") {
        st->st_size = (off_t)ctx.audioSize;
    }
    else if (ctx.dngFileNameToFrameTimestamp.count(name)) {
        st->st_size = (off_t)ctx.frameSize;
    }
    else {
        return -ENOENT;
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
    FSContext &ctx = it->second;

    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);

    // list frames
    for (auto &f : ctx.dngFileNameToFrameTimestamp)
        filler(buf, f.first.c_str(), nullptr, 0);

    // list the audio file
    if (ctx.audioSize) {
        std::string audioName = "audio.wav";
        filler(buf, audioName.c_str(), nullptr, 0);
    }

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
    FSContext &ctx = it->second;

    // allow read‐only audio.wav
    std::string audioName = "audio.wav";
    if (fname == audioName) {
        return (fi->flags & 3) == O_RDONLY ? 0 : -EACCES;
    }

    // otherwise fall through to DNG frames
    if (!ctx.dngFileNameToFrameTimestamp.count(fname))
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
    FSContext &ctx = it->second;

    // if it's the wav file, serve the buffer
    std::string audioName = "audio.wav";
    if (fname == audioName) {
        if ((size_t)offset >= ctx.audioSize)
            return 0;
        size_t tocopy = std::min<size_t>(size, ctx.audioSize - (size_t)offset);
        memcpy(buf, ctx.audioWavData.data() + offset, tocopy);
        return tocopy;
    }

    // otherwise decode & serve a frame
    size_t unneeded = 0;
    int err = load_frame(&ctx, fname, &unneeded);
    if (err < 0)
        return err;
    auto it2 = ctx.dngFileNameToFrameCache.find(fname);
    if (it2 == ctx.dngFileNameToFrameCache.end())
        return -ENOENT;
    auto &data = it2->second;
    if ((size_t)offset >= data.size())
        return 0;
    size_t tocopy = std::min<size_t>(size, data.size() - (size_t)offset);
    memcpy(buf, data.data() + offset, tocopy);
    return tocopy;
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

            FSContext ctx;
            ctx.mcrawBaseName = baseName;
            try {
                // pass the absolute path into the decoder
                ctx.decoder = new motioncam::Decoder(fullPath);
            }
            catch (std::exception &e) {
                std::cerr << "Decoder error (" << fullPath << "): "
                     << e.what() << "\n";
                continue;
            }

            // preload frames + metadata
            auto frameList         = ctx.decoder->getFrames();
            nlohmann::json containerMetadata = ctx.decoder->getContainerMetadata();
            cache_container_metadata(&ctx, &containerMetadata);

            std::cerr << "INFO: [" << fullPath << "] found "
                 << frameList.size() << " frames\n";

            // prepare filename list
            for (size_t i = 0; i < frameList.size(); ++i) {
                ctx.dngFileNameToFrameTimestamp.insert({frameName(baseName, int(i)), frameList[i]});
            }

            // warm up first frame
            if (!ctx.dngFileNameToFrameTimestamp.empty()) {
                load_frame(&ctx, frameName(baseName, 0), &ctx.frameSize);
            }

            // ------------------------------------------------------------------
            // extract & build WAV in memory from the decoder’s audio
            // ------------------------------------------------------------------
            try {
                std::vector<motioncam::AudioChunk> audioChunks;
                std::vector<uint8_t> fileData;
                ctx.decoder->loadAudio(audioChunks);

                int sampleRate  = ctx.decoder->audioSampleRateHz();
                int numChannels = ctx.decoder->numAudioChannels();

                getAudio(
                    fileData,
                    sampleRate,
                    numChannels,
                    audioChunks
                );

                ctx.audioWavData.assign(fileData.begin(), fileData.end());
                ctx.audioSize = ctx.audioWavData.size();
            }
            catch (std::exception &e) {
                std::cerr << "Audio processing error (" << fullPath << "): "
                     << e.what() << "\n";
            }
            // ------------------------------------------------------------------

            // stash context under the base name
            contexts.emplace(baseName, std::move(ctx));
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