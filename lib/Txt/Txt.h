#pragma once

#include <Print.h>

#include <memory>
#include <string>
#include <string_view>

class BookMetadataCache;

class Txt {
 public:
  static bool isTxtOrMd(std::string_view path);
  static bool streamTxtToHtml(const std::string& filepath, Print& out);
  static std::string findCompanionCoverImage(const std::string& filepath);
  static bool buildTxtCache(const std::string& filepath, const std::string& cachePath,
                            std::unique_ptr<BookMetadataCache>& bookMetadataCache);
};
