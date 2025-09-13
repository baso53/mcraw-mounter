#pragma once

#include "motioncam/Decoder.hpp"
#include "IFuseFileSystem.h"
#include "Utils.h"

namespace motioncam {

inline const std::function<void(size_t, int)> EMPTY_CALLBACK =
    [](size_t a, int b) {};

struct CacheEntry {
    Entry entry;
    std::shared_ptr<std::vector<char>> data;
};

class VirtualFileSystemImpl_MCRAW
{
public:
    VirtualFileSystemImpl_MCRAW(
        const std::string& file);

    std::vector<Entry> listFiles(const std::string& filter = "") const;
    std::optional<Entry> findEntry(const std::string& fullPath) const;

    int readFile(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async=true);

    void updateOptions(FileRenderOptions options, int draftScale);
    
    FileInfo getFileInfo() const;

    size_t getTypicalDngSize() {
        return mTypicalDngSize;
    }

    size_t getAudioSize() {
        return mAudioFile.size();
    }

private:
    void init(FileRenderOptions options);

    size_t generateFrame(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async);

    size_t generateAudio(
        const Entry& entry,
        const size_t pos,
        const size_t len,
        void* dst,
        std::function<void(size_t, int)> result,
        bool async);

private:
    const std::string mSrcPath;
    const std::string mBaseName;
    size_t mTypicalDngSize;
    std::vector<Entry> mFiles;
    std::vector<uint8_t> mAudioFile;
    int mDraftScale;
    FileRenderOptions mOptions;
    float mFps;
    int mTotalFrames;
    int mDroppedFrames;
    int mWidth;
    int mHeight;
    std::unique_ptr<Decoder>   sSharedDecoder;
    std::deque<CacheEntry>     mCache;
    static const size_t        MAX_CACHE_SIZE = 5;
};

} // namespace motioncam
