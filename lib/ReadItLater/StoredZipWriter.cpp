#include "StoredZipWriter.h"

#include <Logging.h>

#include <cstring>

namespace {
constexpr uint32_t LOCAL_FILE_SIG = 0x04034b50;
constexpr uint32_t CENTRAL_DIR_SIG = 0x02014b50;
constexpr uint32_t EOCD_SIG = 0x06054b50;
constexpr uint16_t VERSION_NEEDED = 20;
constexpr uint16_t METHOD_STORED = 0;

// IEEE 802.3 CRC-32, reflected, init 0xFFFFFFFF, xorout 0xFFFFFFFF.
// Matches the CRC32 used by ZIP. Table built on first use.
uint32_t crcTable[256];
bool crcTableReady = false;

void buildCrcTable() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) {
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    crcTable[i] = c;
  }
  crcTableReady = true;
}

uint32_t crc32(const uint8_t* data, size_t len) {
  if (!crcTableReady) {
    buildCrcTable();
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c = crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

bool writeLE16(FsFile& f, uint16_t v) {
  uint8_t buf[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF)};
  return f.write(buf, 2) == 2;
}

bool writeLE32(FsFile& f, uint32_t v) {
  uint8_t buf[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                    static_cast<uint8_t>((v >> 16) & 0xFF), static_cast<uint8_t>((v >> 24) & 0xFF)};
  return f.write(buf, 4) == 4;
}
}  // namespace

StoredZipWriter::~StoredZipWriter() {
  if (opened) {
    file.close();
  }
}

bool StoredZipWriter::open(const char* path) {
  if (opened) {
    LOG_ERR("ZIPW", "Already open");
    return false;
  }
  if (!Storage.openFileForWrite("ZIPW", path, file)) {
    return false;
  }
  opened = true;
  entries.reserve(8);
  return true;
}

bool StoredZipWriter::addFile(const char* zipPath, const void* data, size_t size) {
  if (!opened) {
    LOG_ERR("ZIPW", "addFile before open");
    return false;
  }
  if (size > 0xFFFFFFFFu) {
    LOG_ERR("ZIPW", "Entry too large");
    return false;
  }

  Entry e;
  e.path = zipPath;
  e.size = static_cast<uint32_t>(size);
  e.crc32 = crc32(static_cast<const uint8_t*>(data), size);
  e.localOffset = static_cast<uint32_t>(file.position());

  const uint16_t nameLen = static_cast<uint16_t>(e.path.size());

  // Local file header.
  if (!writeLE32(file, LOCAL_FILE_SIG)) return false;
  if (!writeLE16(file, VERSION_NEEDED)) return false;
  if (!writeLE16(file, 0)) return false;              // general purpose flag
  if (!writeLE16(file, METHOD_STORED)) return false;  // compression method
  if (!writeLE16(file, 0)) return false;              // last mod time
  if (!writeLE16(file, 0)) return false;              // last mod date
  if (!writeLE32(file, e.crc32)) return false;
  if (!writeLE32(file, e.size)) return false;  // compressed size == uncompressed for STORED
  if (!writeLE32(file, e.size)) return false;
  if (!writeLE16(file, nameLen)) return false;
  if (!writeLE16(file, 0)) return false;  // extra field length

  if (file.write(e.path.data(), nameLen) != nameLen) return false;
  if (size > 0 && file.write(data, size) != size) return false;

  entries.push_back(std::move(e));
  return true;
}

bool StoredZipWriter::addFileFromPath(const char* zipPath, const char* localPath) {
  if (!opened) {
    LOG_ERR("ZIPW", "addFileFromPath before open");
    return false;
  }
  if (!crcTableReady) {
    buildCrcTable();
  }

  // Pass 1: size + CRC32, streaming.
  FsFile src;
  if (!Storage.openFileForRead("ZIPW", localPath, src)) {
    LOG_ERR("ZIPW", "Cannot open source %s", localPath);
    return false;
  }
  const uint32_t totalSize = static_cast<uint32_t>(src.fileSize());
  uint8_t chunk[256];
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t scanned = 0;
  while (scanned < totalSize) {
    const uint32_t want = totalSize - scanned > sizeof(chunk) ? sizeof(chunk) : totalSize - scanned;
    const int got = src.read(chunk, want);
    if (got <= 0) {
      src.close();
      LOG_ERR("ZIPW", "Short read scanning %s", localPath);
      return false;
    }
    for (int j = 0; j < got; j++) {
      crc = crcTable[(crc ^ chunk[j]) & 0xFF] ^ (crc >> 8);
    }
    scanned += got;
  }
  crc ^= 0xFFFFFFFFu;
  src.close();

  Entry e;
  e.path = zipPath;
  e.size = totalSize;
  e.crc32 = crc;
  e.localOffset = static_cast<uint32_t>(file.position());

  const uint16_t nameLen = static_cast<uint16_t>(e.path.size());

  // Local file header.
  if (!writeLE32(file, LOCAL_FILE_SIG)) return false;
  if (!writeLE16(file, VERSION_NEEDED)) return false;
  if (!writeLE16(file, 0)) return false;
  if (!writeLE16(file, METHOD_STORED)) return false;
  if (!writeLE16(file, 0)) return false;
  if (!writeLE16(file, 0)) return false;
  if (!writeLE32(file, e.crc32)) return false;
  if (!writeLE32(file, e.size)) return false;
  if (!writeLE32(file, e.size)) return false;
  if (!writeLE16(file, nameLen)) return false;
  if (!writeLE16(file, 0)) return false;
  if (file.write(e.path.data(), nameLen) != nameLen) return false;

  // Pass 2: copy bytes.
  if (!Storage.openFileForRead("ZIPW", localPath, src)) {
    LOG_ERR("ZIPW", "Cannot reopen source %s", localPath);
    return false;
  }
  uint32_t copied = 0;
  while (copied < totalSize) {
    const uint32_t want = totalSize - copied > sizeof(chunk) ? sizeof(chunk) : totalSize - copied;
    const int got = src.read(chunk, want);
    if (got <= 0) {
      src.close();
      LOG_ERR("ZIPW", "Short read copying %s", localPath);
      return false;
    }
    if (file.write(chunk, got) != static_cast<size_t>(got)) {
      src.close();
      LOG_ERR("ZIPW", "Short write copying %s", localPath);
      return false;
    }
    copied += got;
  }
  src.close();

  entries.push_back(std::move(e));
  return true;
}

bool StoredZipWriter::addFileFromCallback(const char* zipPath, size_t totalSize, WriteCallback cb) {
  if (!opened) {
    LOG_ERR("ZIPW", "addFileFromCallback before open");
    return false;
  }
  if (!crcTableReady) {
    buildCrcTable();
  }

  // Pass 1: compute CRC32 by running the callback dry into a stack buffer.
  // We call cb twice (pass1 + pass2) so the caller must produce identical
  // bytes on each invocation (i.e. be a pure generator, not a one-shot stream).
  uint32_t crc = 0xFFFFFFFFu;
  size_t scanned = 0;
  uint8_t chunk[256];
  while (scanned < totalSize) {
    const int got = cb(chunk, sizeof(chunk));
    if (got < 0) {
      LOG_ERR("ZIPW", "Callback error during CRC pass");
      return false;
    }
    if (got == 0) break;
    for (int j = 0; j < got; j++) {
      crc = crcTable[(crc ^ chunk[j]) & 0xFF] ^ (crc >> 8);
    }
    scanned += got;
  }
  crc ^= 0xFFFFFFFFu;

  if (scanned != totalSize) {
    LOG_ERR("ZIPW", "Callback produced %zu bytes, expected %zu (CRC pass)", scanned, totalSize);
    return false;
  }

  Entry e;
  e.path = zipPath;
  e.size = static_cast<uint32_t>(totalSize);
  e.crc32 = crc;
  e.localOffset = static_cast<uint32_t>(file.position());

  const uint16_t nameLen = static_cast<uint16_t>(e.path.size());

  // Local file header.
  if (!writeLE32(file, LOCAL_FILE_SIG)) return false;
  if (!writeLE16(file, VERSION_NEEDED)) return false;
  if (!writeLE16(file, 0)) return false;
  if (!writeLE16(file, METHOD_STORED)) return false;
  if (!writeLE16(file, 0)) return false;
  if (!writeLE16(file, 0)) return false;
  if (!writeLE32(file, e.crc32)) return false;
  if (!writeLE32(file, e.size)) return false;
  if (!writeLE32(file, e.size)) return false;
  if (!writeLE16(file, nameLen)) return false;
  if (!writeLE16(file, 0)) return false;
  if (file.write(e.path.data(), nameLen) != nameLen) return false;

  // Pass 2: write bytes.
  size_t copied = 0;
  while (copied < totalSize) {
    const int got = cb(chunk, sizeof(chunk));
    if (got < 0) {
      LOG_ERR("ZIPW", "Callback error during write pass");
      return false;
    }
    if (got == 0) break;
    if (file.write(chunk, got) != static_cast<size_t>(got)) {
      LOG_ERR("ZIPW", "Short write during callback copy");
      return false;
    }
    copied += got;
  }

  if (copied != totalSize) {
    LOG_ERR("ZIPW", "Callback produced %zu bytes, expected %zu (write pass)", copied, totalSize);
    return false;
  }

  entries.push_back(std::move(e));
  return true;
}

bool StoredZipWriter::finish() {
  if (!opened) {
    return false;
  }

  const uint32_t centralDirOffset = static_cast<uint32_t>(file.position());

  // Central directory headers.
  for (const Entry& e : entries) {
    const uint16_t nameLen = static_cast<uint16_t>(e.path.size());
    if (!writeLE32(file, CENTRAL_DIR_SIG)) goto fail;
    if (!writeLE16(file, VERSION_NEEDED)) goto fail;  // version made by
    if (!writeLE16(file, VERSION_NEEDED)) goto fail;  // version needed
    if (!writeLE16(file, 0)) goto fail;               // flags
    if (!writeLE16(file, METHOD_STORED)) goto fail;
    if (!writeLE16(file, 0)) goto fail;  // mod time
    if (!writeLE16(file, 0)) goto fail;  // mod date
    if (!writeLE32(file, e.crc32)) goto fail;
    if (!writeLE32(file, e.size)) goto fail;
    if (!writeLE32(file, e.size)) goto fail;
    if (!writeLE16(file, nameLen)) goto fail;
    if (!writeLE16(file, 0)) goto fail;  // extra
    if (!writeLE16(file, 0)) goto fail;  // comment
    if (!writeLE16(file, 0)) goto fail;  // disk number
    if (!writeLE16(file, 0)) goto fail;  // internal attrs
    if (!writeLE32(file, 0)) goto fail;  // external attrs
    if (!writeLE32(file, e.localOffset)) goto fail;
    if (file.write(e.path.data(), nameLen) != nameLen) goto fail;
  }

  {
    const uint32_t centralDirSize = static_cast<uint32_t>(file.position()) - centralDirOffset;
    const uint16_t entryCount = static_cast<uint16_t>(entries.size());

    if (!writeLE32(file, EOCD_SIG)) goto fail;
    if (!writeLE16(file, 0)) goto fail;  // disk number
    if (!writeLE16(file, 0)) goto fail;  // disk with central dir
    if (!writeLE16(file, entryCount)) goto fail;
    if (!writeLE16(file, entryCount)) goto fail;
    if (!writeLE32(file, centralDirSize)) goto fail;
    if (!writeLE32(file, centralDirOffset)) goto fail;
    if (!writeLE16(file, 0)) goto fail;  // comment length
  }

  file.flush();
  file.close();
  opened = false;
  return true;

fail:
  LOG_ERR("ZIPW", "Write failed during finish");
  file.close();
  opened = false;
  return false;
}
