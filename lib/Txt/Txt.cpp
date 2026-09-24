#include "Txt.h"

#include <BufferedFile.h>
#include <Epub/BookMetadataCache.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

bool Txt::isTxtOrMd(std::string_view path) {
  return FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path);
}

std::string Txt::findCompanionCoverImage(const std::string& filepath) {
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) folder = "/";

  std::string baseName = FsHelpers::getFileNameWithoutExtension(filepath);
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) return coverPath;
  }

  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) return coverPath;
    }
  }

  return "";
}

bool Txt::streamTxtToHtml(const std::string& filepath, Print& out) {
  const uint32_t t0 = millis();
  HalFile src;
  if (!Storage.openFileForRead("TXT", filepath, src)) {
    LOG_ERR("TXT", "Failed to open TXT/MD for streaming: %s", filepath.c_str());
    return false;
  }

  const size_t srcSize = src.size();
  LOG_DBG("TXT", "Converting TXT/MD to HTML: %s (%zu bytes)", filepath.c_str(), srcSize);

  constexpr size_t IN_BUF_SIZE = 8192;
  constexpr size_t OUT_BUF_SIZE = 8192;

  auto inBuf = makeUniqueNoThrow<uint8_t[]>(IN_BUF_SIZE);
  auto outBuf = makeUniqueNoThrow<uint8_t[]>(OUT_BUF_SIZE);
  if (!inBuf || !outBuf) {
    LOG_ERR("TXT", "OOM: TXT/MD HTML streaming buffers");
    return false;
  }

  size_t outPos = 0;
  size_t totalBytesOut = 0;
  auto flushOut = [&]() {
    if (outPos > 0) {
      out.write(outBuf.get(), outPos);
      totalBytesOut += outPos;
      outPos = 0;
    }
  };

  auto writeByte = [&](uint8_t b) {
    outBuf[outPos++] = b;
    if (outPos == OUT_BUF_SIZE) flushOut();
  };

  auto writeStr = [&](std::string_view s) {
    for (char c : s) {
      writeByte(static_cast<uint8_t>(c));
    }
  };

  writeStr("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!DOCTYPE html>\n<html>\n<head><title>");
  std::string title = FsHelpers::getFileNameWithoutExtension(filepath);
  for (char c : title) {
    if (c == '&')
      writeStr("&amp;");
    else if (c == '<')
      writeStr("&lt;");
    else if (c == '>')
      writeStr("&gt;");
    else
      writeByte(static_cast<uint8_t>(c));
  }
  writeStr("</title></head>\n<body>\n<p>");

  bool isStart = true;
  bool startedParagraphText = false;
  int consecutiveNewlines = 0;
  int bytesRead = 0;

  while ((bytesRead = src.read(inBuf.get(), IN_BUF_SIZE)) > 0) {
    int startIdx = 0;
    if (isStart) {
      isStart = false;
      if (bytesRead >= 3 && inBuf[0] == 0xEF && inBuf[1] == 0xBB && inBuf[2] == 0xBF) {
        startIdx = 3;
      }
    }

    for (int i = startIdx; i < bytesRead; i++) {
      uint8_t b = inBuf[i];
      if (b == '\r') continue;
      if (b == '\n') {
        consecutiveNewlines++;
        if (consecutiveNewlines == 2 && startedParagraphText) {
          writeStr("</p>\n<p>");
          startedParagraphText = false;
        }
        continue;
      }

      if (consecutiveNewlines == 1 && startedParagraphText) {
        writeByte(' ');
      }
      consecutiveNewlines = 0;
      startedParagraphText = true;

      if (b == '&') {
        writeStr("&amp;");
      } else if (b == '<') {
        writeStr("&lt;");
      } else if (b == '>') {
        writeStr("&gt;");
      } else if (b < 0x20 && b != '\t') {
        writeByte(' ');
      } else {
        writeByte(b);
      }
    }
  }

  writeStr("</p>\n</body>\n</html>\n");
  flushOut();
  LOG_DBG("TXT", "Converted TXT/MD to HTML in %lu ms (%zu bytes in -> %zu bytes out)", millis() - t0, srcSize,
          totalBytesOut);
  return true;
}

bool Txt::buildTxtCache(const std::string& filepath, const std::string& cachePath,
                        std::unique_ptr<BookMetadataCache>& bookMetadataCache) {
  LOG_DBG("TXT", "Building metadata cache for TXT: %s", filepath.c_str());

  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }

  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("TXT", "Could not begin writing cache");
    return false;
  }

  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("TXT", "Could not begin writing content.opf pass");
    return false;
  }

  bookMetadataCache->createSpineEntry("content.html");

  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("TXT", "Could not end writing content.opf pass");
    return false;
  }

  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("TXT", "Could not begin writing toc pass");
    return false;
  }

  std::string title = FsHelpers::getFileNameWithoutExtension(filepath);
  bookMetadataCache->createTocEntry(title, "content.html", "", 0);

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("TXT", "Could not end writing toc pass");
    return false;
  }

  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("TXT", "Could not end writing cache");
    return false;
  }

  BookMetadataCache::BookMetadata bookMetadata;
  bookMetadata.title = utf8ComposeNfc(title);
  bookMetadata.language = "en";

  std::string companionCover = findCompanionCoverImage(filepath);
  if (!companionCover.empty()) {
    bookMetadata.coverItemHref = companionCover;
  }

  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("TXT", "Could not build book.bin for TXT");
    return false;
  }

  bookMetadataCache->cleanupTmpFiles();

  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR("TXT", "Failed to reload cache after build");
    return false;
  }

  return true;
}
