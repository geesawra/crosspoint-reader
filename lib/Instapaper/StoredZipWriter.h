#pragma once
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * Minimal ZIP writer that emits only STORED (uncompressed) entries.
 *
 * Rationale: the project links an inflater (InflateReader) but no deflate
 * encoder and no miniz. STORED is enough for our use case because:
 *   (1) the EPUB spec requires the `mimetype` entry to be STORED anyway,
 *   (2) article XHTML is small (tens of KB), so deflate would save little,
 *   (3) it keeps the writer under ~150 lines with no extra flash cost.
 *
 * Usage:
 *   StoredZipWriter zip;
 *   if (!zip.open("/path/to/out.epub")) return;
 *   zip.addFile("mimetype", mimeBytes, mimeLen);
 *   zip.addFile("META-INF/container.xml", containerBytes, containerLen);
 *   zip.addFile("OEBPS/content.opf", opfBytes, opfLen);
 *   zip.addFile("OEBPS/article.xhtml", xhtmlBytes, xhtmlLen);
 *   zip.finish();
 */
class StoredZipWriter {
 public:
  StoredZipWriter() = default;
  ~StoredZipWriter();

  StoredZipWriter(const StoredZipWriter&) = delete;
  StoredZipWriter& operator=(const StoredZipWriter&) = delete;

  // Open output file for writing. Returns false on SD failure.
  bool open(const char* path);

  // Append an entry. `zipPath` is the archive-relative path (e.g. "OEBPS/article.xhtml").
  // Returns false on SD failure or if the archive is not open.
  bool addFile(const char* zipPath, const void* data, size_t size);

  // Streaming variant: appends an entry whose payload lives in another file
  // on SD. Reads the source file twice (once for CRC32, once to copy bytes)
  // in small chunks, so peak heap stays bounded no matter how large the
  // input. Needed for embedding article images without loading them fully
  // into memory (they routinely exceed 100 KB, which is near our free-heap
  // ceiling during EPUB synthesis).
  bool addFileFromPath(const char* zipPath, const char* localPath);

  // Write the central directory + EOCD and close the file. Must be called to
  // produce a valid ZIP. After this, the writer is in a closed state.
  bool finish();

 private:
  struct Entry {
    std::string path;
    uint32_t crc32 = 0;
    uint32_t size = 0;
    uint32_t localOffset = 0;
  };

  FsFile file;
  std::vector<Entry> entries;
  bool opened = false;
};
