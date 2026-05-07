# Plan: Recursive File/Directory Deletion

## Context
The web UI (`CrossPointWebServer`) and on-device UI (`FileBrowserActivity`) both support file deletion, but only for individual files or **empty** directories. The underlying `SDCardManager::removeDir()` already supports recursive directory deletion, and `HalStorage::removeDir()` wraps it. We need to:
1. Enable recursive directory deletion from the **web UI**
2. Enable recursive directory deletion from the **device itself** (FileBrowserActivity)
3. Share as much code as possible between these paths
4. Ensure `.epub` cache clearing is handled consistently during recursive deletes

## Current State

### Web UI deletion (`src/network/CrossPointWebServer.cpp::handleDelete()`)
- Accepts `path` or `paths` (JSON array)
- Security checks: rejects root `/`, hidden files (`.` prefix), and protected items (`System Volume Information`, `XTCache`)
- For directories: opens dir, checks if empty, calls `Storage.rmdir()` — **rejects non-empty dirs**
- For files: calls `Storage.remove()` + `clearEpubCacheIfNeeded()`
- `clearEpubCacheIfNeeded()` is duplicated in `CrossPointWebServer.cpp` and `WebDAVHandler.cpp`

### WebDAV deletion (`src/network/WebDAVHandler.cpp::handleDelete()`)
- Same pattern: rejects non-empty directories
- Has its own copy of `clearEpubCacheIfNeeded()`

### On-device deletion (`src/activities/home/FileBrowserActivity.cpp`)
- Long-press Confirm on a **file** triggers `ConfirmationActivity`, then calls `Storage.remove()` + `clearFileMetadata()` (same as epub cache clear)
- Long-press on a **directory** currently falls through to short-press (navigate into dir)
- No protection against `XTCache` (though `loadFiles()` filters out hidden files and `System Volume Information`)

### Existing recursive deletion
- `SDCardManager::removeDir()` in `open-x4-sdk` recursively deletes directories
- `ClearCacheActivity` already uses `Storage.removeDir()` to wipe cache trees

## Proposed Approach

### 1. Create a shared deletion utility
Add `src/util/FileDeletionUtil.h` + `.cpp` containing:
- `bool deletePath(const char* path)` — recursively deletes a file or directory
  - If file: clears epub cache if `.epub`, then `Storage.remove()`
  - If directory: recursively walks contents, clears epub cache for any `.epub` files inside, then deletes the directory tree using `Storage.removeDir()` (or manual recursion if we want single-walk cache clearing)
- `void clearEpubCacheIfNeeded(const std::string& path)` — extracted shared helper

This avoids creating circular dependencies (Epub.cpp already includes FsHelpers.h) and keeps the deletion logic in one place.

### 2. Update web UI `/delete` endpoint
- Replace manual file/dir logic in `handleDelete()` with `deletePath()`
- Keep existing security checks (root, hidden, protected items)
- Remove local `clearEpubCacheIfNeeded()` function

### 3. Update WebDAV `DELETE`
- Replace manual file/dir logic with `deletePath()`
- Remove local `clearEpubCacheIfNeeded()` method

### 4. Update on-device `FileBrowserActivity`
- Remove `&& !isDirectory` guard on long-press Confirm
- Use `deletePath()` instead of `Storage.remove()`
- Remove `clearFileMetadata()` method (replaced by shared helper)
- Add protection against deleting `XTCache` (mirroring web UI)
- Potentially adjust confirmation heading for directories vs files

## Open Questions

1. **WebDAV scope**: Should recursive deletion also be enabled for WebDAV `DELETE`, or is that out of scope?
2. **On-device protected items**: Should `FileBrowserActivity` prevent deleting `XTCache` (like the web UI does), or is the existing hidden-file filter sufficient?
3. **i18n for directory deletion**: Reuse existing `STR_DELETE` ("Delete? foldername") for both files and folders, or add a new translatable string like "Delete folder and contents?" (requires editing all 22 translation YAMLs and running `gen_i18n.py`)?

## Files to Modify
- `src/util/FileDeletionUtil.h` (new)
- `src/util/FileDeletionUtil.cpp` (new)
- `src/network/CrossPointWebServer.cpp`
- `src/network/WebDAVHandler.cpp` (if including WebDAV)
- `src/network/WebDAVHandler.h` (if including WebDAV)
- `src/activities/home/FileBrowserActivity.cpp`
- `src/activities/home/FileBrowserActivity.h`
- `lib/I18n/translations/*.yaml` (if adding new i18n string)

## Reuse
- `Storage.removeDir()` — `lib/hal/HalStorage.cpp` / `open-x4-sdk/libs/hardware/SDCardManager.cpp` (existing recursive deletion)
- `Epub::clearCache()` — `lib/Epub/Epub.cpp` (existing cache clearing)
- `ConfirmationActivity` — `src/activities/util/ConfirmationActivity.h` (existing UI pattern)
- `FsHelpers::hasEpubExtension()` — `lib/FsHelpers/FsHelpers.cpp`

## Verification
- Web UI: Test deleting a non-empty directory via `/delete` endpoint
- Web UI: Test deleting multiple paths including a mix of files and directories
- On-device: Test long-press Confirm on a directory in FileBrowser
- On-device: Test deleting a directory containing `.epub` files (verify cache is cleared)
- WebDAV: Test `DELETE` on a non-empty directory (if included)
