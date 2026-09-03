// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "cache.hh"

#include "config.hh"
#include "indexer.hh"
#include "log.hh"
#include "serializer.hh"
#include "utils.hh"

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

#include <shared_mutex>
#include <unordered_map>

using namespace llvm;

namespace ccls {
namespace {
struct InMemoryIndexFile {
  std::string content;
  IndexFile index;
};

constexpr StringLiteral cacheMagic("ccls-cache-v2");

std::string digest(StringRef content) {
  auto bytes = arrayRefFromStringRef(content);
  return toHex(SHA256::hash(bytes), true);
}

std::string shardedPath(StringRef kind, StringRef key) {
  SmallString<256> path(g_config->cache.directory);
  sys::path::append(path, "v2", kind, key.substr(0, 2), key);
  return std::string(path);
}

std::string referencePath(const std::string &source) { return shardedPath("refs", digest(source)); }
std::string objectPath(StringRef key) { return shardedPath("objects", key); }

std::optional<std::string> referencedObject(const std::string &source) {
  std::optional<std::string> reference = readContent(referencePath(source));
  if (!reference)
    return {};
  StringRef key = StringRef(*reference).trim();
  if (key.size() != 64)
    return {};
  for (char c : key)
    if (!isHexDigit(c))
      return {};
  return key.str();
}

StringRef formatName() {
  switch (g_config->cache.format) {
  case SerializeFormat::Binary:
    return "binary";
  case SerializeFormat::Json:
    return "json";
  }
}

std::optional<SerializeFormat> parseFormat(StringRef name) {
  if (name == "binary")
    return SerializeFormat::Binary;
  if (name == "json")
    return SerializeFormat::Json;
  return {};
}

std::string encodeEntry(IndexFile &file) {
  std::string index = serialize(g_config->cache.format, file);
  std::string result;
  raw_string_ostream stream(result);
  stream << cacheMagic << '\n' << formatName() << '\n' << file.file_contents.size() << '\n' << index.size() << '\n';
  stream << file.file_contents << index;
  return result;
}

struct DecodedEntry {
  SerializeFormat format;
  StringRef content;
  StringRef index;
};

std::optional<DecodedEntry> decodeEntry(StringRef encoded) {
  auto line = [&]() -> std::optional<StringRef> {
    size_t newline = encoded.find('\n');
    if (newline == StringRef::npos)
      return {};
    StringRef result = encoded.take_front(newline);
    encoded = encoded.drop_front(newline + 1);
    return result;
  };

  auto magic = line();
  auto format_line = line();
  auto content_size_line = line();
  auto index_size_line = line();
  if (!magic || *magic != cacheMagic || !format_line || !content_size_line || !index_size_line)
    return {};
  auto format = parseFormat(*format_line);
  uint64_t content_size, index_size;
  if (!format || content_size_line->getAsInteger(10, content_size) || index_size_line->getAsInteger(10, index_size) ||
      content_size > encoded.size() || index_size > encoded.size() - content_size ||
      content_size + index_size != encoded.size())
    return {};
  return DecodedEntry{*format, encoded.take_front(content_size), encoded.drop_front(content_size)};
}

// Publish a complete object or reference with rename instead of exposing a
// partially written cache entry to another reader.
bool writeAtomically(const std::string &path, const std::string &content) {
  if (std::error_code ec = sys::fs::create_directories(sys::path::parent_path(path))) {
    LOG_S(ERROR) << "failed to create cache directory for " << path << ' ' << ec.message();
    return false;
  }
  SmallString<256> model(path);
  model += ".tmp-%%%%%%";
  SmallString<256> temporary;
  int fd;
  if (std::error_code ec = sys::fs::createUniqueFile(model, fd, temporary)) {
    LOG_S(ERROR) << "failed to create cache temporary for " << path << ' ' << ec.message();
    return false;
  }

  {
    raw_fd_ostream stream(fd, true);
    stream << content;
    stream.flush();
    if (stream.has_error()) {
      LOG_S(ERROR) << "failed to write to " << temporary.c_str();
      stream.clear_error();
      (void)sys::fs::remove(temporary);
      return false;
    }
  }
  if (std::error_code ec = sys::fs::rename(temporary, path)) {
    LOG_S(ERROR) << "failed to publish cache file " << path << ' ' << ec.message();
    (void)sys::fs::remove(temporary);
    return false;
  }
  return true;
}
} // namespace

struct IndexCache::Impl {
  static constexpr int mutex_count = 256;
  std::mutex mutexes[mutex_count];
  std::shared_mutex memory_mutex;
  std::unordered_map<std::string, InMemoryIndexFile> memory;
};

IndexCache::IndexCache() : impl(std::make_unique<Impl>()) {}
IndexCache::~IndexCache() = default;

std::mutex &IndexCache::mutex(const std::string &path) {
  return impl->mutexes[std::hash<std::string>()(path) % Impl::mutex_count];
}

std::unique_ptr<IndexFile> IndexCache::load(const std::string &path) {
  if (g_config->cache.retainInMemory) {
    std::shared_lock lock(impl->memory_mutex);
    auto it = impl->memory.find(path);
    if (it != impl->memory.end())
      return std::make_unique<IndexFile>(it->second.index);
    if (g_config->cache.directory.empty())
      return nullptr;
  }

  std::optional<std::string> object_key = referencedObject(path);
  if (!object_key)
    return nullptr;
  std::optional<std::string> encoded = readContent(objectPath(*object_key));
  if (!encoded)
    return nullptr;
  auto entry = decodeEntry(*encoded);
  if (!entry || entry->format != g_config->cache.format)
    return nullptr;
  return deserialize(entry->format, path, entry->index.str(), entry->content.str(), IndexFile::kMajorVersion);
}

void IndexCache::store(IndexFile &file, int previous_loads, bool deleted) {
  const std::string &path = file.path;
  int retain = g_config->cache.retainInMemory;
  if (retain > 0 && retain <= previous_loads + 1) {
    std::lock_guard lock(impl->memory_mutex);
    auto it = impl->memory.insert_or_assign(path, InMemoryIndexFile{file.file_contents, file});
    std::string().swap(it.first->second.index.file_contents);
  }

  if (g_config->cache.directory.empty())
    return;

  std::string reference_path = referencePath(path);
  if (deleted) {
    (void)sys::fs::remove(reference_path);
    return;
  }
  std::string encoded = encodeEntry(file);
  std::string object_key = digest(encoded);
  std::string object_path = objectPath(object_key);
  if (!sys::fs::exists(object_path) && !writeAtomically(object_path, encoded))
    return;
  (void)writeAtomically(reference_path, object_key + '\n');
}

void IndexCache::removeMemory(const std::string &path) {
  // A disk-backed cache can reconstruct a retained index after a document
  // closes. Memory-only mode keeps the index because it has no other copy.
  if (g_config->cache.directory.empty())
    return;
  std::lock_guard lock(impl->memory_mutex);
  impl->memory.erase(path);
}

std::optional<std::string> IndexCache::loadIndexedContent(const std::string &path) {
  if (g_config->cache.directory.empty()) {
    std::shared_lock lock(impl->memory_mutex);
    auto it = impl->memory.find(path);
    if (it == impl->memory.end())
      return {};
    return it->second.content;
  }
  std::optional<std::string> object_key = referencedObject(path);
  if (!object_key)
    return {};
  std::optional<std::string> encoded = readContent(objectPath(*object_key));
  if (!encoded)
    return {};
  auto entry = decodeEntry(*encoded);
  if (!entry)
    return {};
  return entry->content.str();
}

IndexCache &indexCache() {
  static IndexCache cache;
  return cache;
}
} // namespace ccls
