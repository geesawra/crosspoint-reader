#pragma once

#include <string>

namespace FileDeletionUtil {

/**
 * Recursively delete a file or directory at the given path.
 * For .epub files, clears the associated reading cache before deletion.
 * Returns true on success, false on failure.
 */
bool deletePath(const char* path);
inline bool deletePath(const std::string& path) { return deletePath(path.c_str()); }

/**
 * Clear the epub reading cache for the given file path if it has a .epub extension.
 */
void clearEpubCacheIfNeeded(const std::string& path);

}  // namespace FileDeletionUtil
