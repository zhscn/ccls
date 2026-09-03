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
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <limits>
#include <shared_mutex>
#include <stdlib.h>
#include <unordered_map>

using namespace llvm;

namespace ccls {
namespace {
struct InMemoryIndexFile {
  std::string content;
  IndexFile index;
};

constexpr StringLiteral cacheMagic("ccls-cache-v3");

std::string digest(StringRef content) {
  auto bytes = arrayRefFromStringRef(content);
  return toHex(SHA256::hash(bytes), true);
}

std::string canonicalPath(StringRef base, StringRef path) {
  SmallString<256> result;
  if (sys::path::is_absolute(path))
    result = path;
  else {
    result = base;
    sys::path::append(result, path);
  }
  sys::path::remove_dots(result, true);
  std::string real = realPath(std::string(result));
  return real.empty() ? std::string(result) : real;
}

struct RepositoryLayout {
  std::string worktree;
  std::string common_dir;
  std::string identity;
};

std::optional<RepositoryLayout> repositoryLayout(std::string start) {
  start = canonicalPath("", start);
  while (!start.empty()) {
    SmallString<256> dot_git(start);
    sys::path::append(dot_git, ".git");
    sys::fs::file_status status;
    if (!sys::fs::status(dot_git, status)) {
      std::string git_dir;
      if (sys::fs::is_directory(status)) {
        git_dir = canonicalPath("", dot_git);
      } else if (sys::fs::is_regular_file(status)) {
        std::optional<std::string> content = readContent(std::string(dot_git));
        if (!content)
          return {};
        StringRef value(*content);
        value = value.trim();
        if (!value.consume_front("gitdir:"))
          return {};
        git_dir = canonicalPath(start, value.trim());
      }
      if (!git_dir.empty()) {
        std::string common_dir = git_dir;
        SmallString<256> commondir(git_dir);
        sys::path::append(commondir, "commondir");
        if (std::optional<std::string> content = readContent(std::string(commondir)))
          common_dir = canonicalPath(git_dir, StringRef(*content).trim());
        return RepositoryLayout{start, common_dir, digest(common_dir)};
      }
    }
    StringRef parent = sys::path::parent_path(start);
    if (parent.empty() || parent == start)
      break;
    start = parent.str();
  }
  return {};
}

std::string cacheHome() {
  if (const char *xdg = getenv("XDG_CACHE_HOME"); xdg && *xdg)
    return canonicalPath("", xdg);
  if (const char *home = getenv("HOME"); home && *home)
    return canonicalPath(home, ".cache");
  return {};
}

struct WorkspaceIdentity {
  std::string root;
  std::string repository;
};

std::mutex repository_identity_mutex;
std::unordered_map<std::string, std::string> repository_identities;

WorkspaceIdentity workspaceIdentity(std::string root) {
  while (root.size() > 1 && root.back() == '/')
    root.pop_back();
  {
    std::lock_guard lock(repository_identity_mutex);
    auto it = repository_identities.find(root);
    if (it != repository_identities.end())
      return {root, it->second};
  }
  auto repository = repositoryLayout(root);
  std::string identity = repository ? repository->identity : digest(root);
  {
    std::lock_guard lock(repository_identity_mutex);
    repository_identities.insert_or_assign(root, identity);
  }
  return {std::move(root), std::move(identity)};
}

bool isUnder(StringRef path, StringRef root) {
  return path == root || (path.startswith(root) && path.size() > root.size() && path[root.size()] == '/');
}

std::optional<WorkspaceIdentity> workspaceFor(const std::string &path, const std::vector<const char *> &args) {
  for (auto &[folder, _] : g_config->workspaceFolders) {
    WorkspaceIdentity workspace = workspaceIdentity(folder);
    if (isUnder(path, workspace.root))
      return workspace;
  }
  for (auto &[folder, _] : g_config->workspaceFolders) {
    WorkspaceIdentity workspace = workspaceIdentity(folder);
    for (const char *arg : args)
      if (StringRef(arg).contains(workspace.root))
        return workspace;
  }
  return {};
}

std::string replaceRoot(std::string value, StringRef from, StringRef to) {
  if (from.empty() || from == to)
    return value;
  for (size_t position = 0; (position = value.find(from.str(), position)) != std::string::npos;) {
    value.replace(position, from.size(), to.str());
    position += to.size();
  }
  return value;
}

std::string logicalPath(const std::string &path, const std::optional<WorkspaceIdentity> &workspace) {
  if (!workspace)
    return "external:" + path;
  if (isUnder(path, workspace->root))
    return "repository:" + workspace->repository + ':' + path.substr(workspace->root.size());
  return "repository:" + workspace->repository + ":external:" + path;
}

std::string referenceKey(const std::string &path, const std::vector<const char *> &args) {
  std::optional<WorkspaceIdentity> workspace = workspaceFor(path, args);
  std::string key = logicalPath(path, workspace);
  key.push_back('\0');
  key += "schema:" + std::to_string(IndexFile::kMajorVersion) + '.' + std::to_string(IndexFile::kMinorVersion);
  key.push_back('\0');
  key += "llvm:" LLVM_VERSION_STRING;
  key.push_back('\0');
  key += "index:" + std::to_string(g_config->index.comments) + ':' +
         std::to_string(g_config->index.maxInitializerLines) + ':' + std::to_string(g_config->index.multiVersion) +
         ':' + std::to_string(g_config->index.name.suppressUnwrittenScope) + ':' +
         std::to_string(g_config->index.parametersInDeclarations);
  key.push_back('\0');
  for (const char *arg : args) {
    key += workspace ? replaceRoot(arg, workspace->root, "${WORKTREE}") : std::string(arg);
    key.push_back('\0');
  }
  return digest(key);
}

std::string contentReferenceKey(const std::string &path, const std::vector<const char *> &args) {
  return digest(std::string("content\0", 8) + logicalPath(path, workspaceFor(path, args)));
}

std::string shardedPath(StringRef kind, StringRef key) {
  SmallString<256> path(g_config->cache.directory);
  sys::path::append(path, "v3", kind, key.substr(0, 2), key);
  return std::string(path);
}

std::string referencePath(StringRef key) { return shardedPath("refs", key); }
std::string objectPath(StringRef key) { return shardedPath("objects", key); }

std::optional<std::string> referencedObject(StringRef reference_key) {
  std::optional<std::string> reference = readContent(referencePath(reference_key));
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

struct DependencyDigest {
  std::string path;
  std::string digest;
};

struct FileDigestMemo {
  sys::TimePoint<> mtime;
  uint64_t size;
  std::string digest;
};

std::mutex file_digest_mutex;
std::unordered_map<std::string, FileDigestMemo> file_digests;

std::optional<std::string> fileDigest(const std::string &path) {
  sys::fs::file_status before;
  if (sys::fs::status(path, before))
    return {};
  {
    std::lock_guard lock(file_digest_mutex);
    auto it = file_digests.find(path);
    if (it != file_digests.end() && it->second.mtime == before.getLastModificationTime() &&
        it->second.size == before.getSize())
      return it->second.digest;
  }
  std::optional<std::string> content = readContent(path);
  if (!content)
    return {};
  sys::fs::file_status after;
  if (sys::fs::status(path, after) || before.getLastModificationTime() != after.getLastModificationTime() ||
      before.getSize() != after.getSize())
    return {};
  std::string value = digest(*content);
  {
    std::lock_guard lock(file_digest_mutex);
    file_digests.insert_or_assign(path, FileDigestMemo{after.getLastModificationTime(), after.getSize(), value});
  }
  return value;
}

std::string encodeEntry(IndexFile &file, StringRef origin) {
  std::string index = serialize(g_config->cache.format, file);
  std::vector<DependencyDigest> dependencies;
  dependencies.reserve(file.dependencies.size());
  for (const auto &dependency : file.dependencies) {
    std::string path = dependency.first.val().str();
    std::optional<std::string> value = fileDigest(path);
    if (!value)
      return {};
    dependencies.push_back({std::move(path), std::move(*value)});
  }
  std::sort(dependencies.begin(), dependencies.end(),
            [](const auto &left, const auto &right) { return left.path < right.path; });

  std::string result;
  raw_string_ostream stream(result);
  stream << cacheMagic << '\n'
         << formatName() << '\n'
         << origin.size() << '\n'
         << file.file_contents.size() << '\n'
         << index.size() << '\n'
         << dependencies.size() << '\n';
  for (const auto &dependency : dependencies)
    stream << dependency.path.size() << '\n' << dependency.digest << '\n';
  stream << origin;
  for (const auto &dependency : dependencies)
    stream << dependency.path;
  stream << file.file_contents << index;
  return result;
}

struct DecodedEntry {
  SerializeFormat format;
  StringRef origin;
  std::vector<std::pair<StringRef, StringRef>> dependencies;
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
  auto origin_size_line = line();
  auto content_size_line = line();
  auto index_size_line = line();
  auto dependency_count_line = line();
  if (!magic || *magic != cacheMagic || !format_line || !origin_size_line || !content_size_line || !index_size_line ||
      !dependency_count_line)
    return {};
  auto format = parseFormat(*format_line);
  uint64_t origin_size, content_size, index_size, dependency_count;
  if (!format || origin_size_line->getAsInteger(10, origin_size) || content_size_line->getAsInteger(10, content_size) ||
      index_size_line->getAsInteger(10, index_size) || dependency_count_line->getAsInteger(10, dependency_count) ||
      dependency_count > encoded.size())
    return {};
  std::vector<std::pair<uint64_t, StringRef>> dependency_headers;
  dependency_headers.reserve(dependency_count);
  uint64_t payload_size = origin_size;
  auto add_size = [&](uint64_t size) {
    if (size > std::numeric_limits<uint64_t>::max() - payload_size)
      return false;
    payload_size += size;
    return true;
  };
  if (!add_size(content_size) || !add_size(index_size))
    return {};
  for (uint64_t i = 0; i < dependency_count; ++i) {
    auto path_size_line = line();
    auto digest_line = line();
    uint64_t path_size;
    if (!path_size_line || !digest_line || digest_line->size() != 64 || path_size_line->getAsInteger(10, path_size))
      return {};
    for (char c : *digest_line)
      if (!isHexDigit(c))
        return {};
    dependency_headers.push_back({path_size, *digest_line});
    if (!add_size(path_size))
      return {};
  }
  if (payload_size != encoded.size())
    return {};

  DecodedEntry result{*format, {}, {}, {}, {}};
  result.origin = encoded.take_front(origin_size);
  encoded = encoded.drop_front(origin_size);
  result.dependencies.reserve(dependency_count);
  for (auto &[path_size, dependency_digest] : dependency_headers) {
    result.dependencies.push_back({encoded.take_front(path_size), dependency_digest});
    encoded = encoded.drop_front(path_size);
  }
  result.content = encoded.take_front(content_size);
  result.index = encoded.drop_front(content_size);
  return result;
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

std::string rebasePath(std::string path, StringRef origin, StringRef current) {
  if (origin.empty() || current.empty() || origin == current)
    return path;
  if (isUnder(path, origin))
    return current.str() + path.substr(origin.size());
  return path;
}

bool validateEntry(const DecodedEntry &entry, const std::string &path, StringRef current_root) {
  std::optional<std::string> current_content = readContent(path);
  if (!current_content || *current_content != entry.content)
    return false;
  for (auto &[stored_path, stored_digest] : entry.dependencies) {
    std::string current_path = rebasePath(stored_path.str(), entry.origin, current_root);
    std::optional<std::string> current_digest = fileDigest(current_path);
    if (!current_digest || *current_digest != stored_digest)
      return false;
  }
  return true;
}

void rebaseIndex(IndexFile &file, StringRef origin, StringRef current) {
  if (!origin.empty() && !current.empty() && origin != current) {
    file.import_file = rebasePath(std::move(file.import_file), origin, current);
    std::vector<const char *> args;
    args.reserve(file.args.size());
    for (const char *arg : file.args)
      args.push_back(intern(replaceRoot(arg, origin, current)));
    file.args = std::move(args);
    for (auto &[_, path] : file.lid2path)
      path = rebasePath(std::move(path), origin, current);
    for (auto &include : file.includes)
      include.resolved_path = intern(rebasePath(include.resolved_path, origin, current));
  }

  if (std::optional<int64_t> mtime = lastWriteTime(file.path))
    file.mtime = *mtime;
  decltype(file.dependencies) dependencies;
  for (auto &dependency : file.dependencies) {
    std::string path = rebasePath(dependency.first.val().str(), origin, current);
    if (std::optional<int64_t> mtime = lastWriteTime(path))
      dependencies[internH(path)] = *mtime;
  }
  file.dependencies = std::move(dependencies);
}
} // namespace

std::string resolveCacheDirectory(const std::string &project_root, const std::string &configured) {
  if (configured != "auto")
    return configured;
  std::string base = cacheHome();
  if (base.empty())
    return canonicalPath(project_root, ".cache/ccls");
  auto repository = repositoryLayout(project_root);
  std::string identity = repository ? repository->identity : digest(canonicalPath("", project_root));
  SmallString<256> path(base);
  sys::path::append(path, "ccls", "repositories", identity);
  return std::string(path);
}

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

std::unique_ptr<IndexFile> IndexCache::load(const std::string &path, const std::vector<const char *> &args) {
  if (g_config->cache.retainInMemory) {
    std::shared_lock lock(impl->memory_mutex);
    auto it = impl->memory.find(path);
    if (it != impl->memory.end())
      return std::make_unique<IndexFile>(it->second.index);
    if (g_config->cache.directory.empty())
      return nullptr;
  }

  std::optional<WorkspaceIdentity> workspace = workspaceFor(path, args);
  std::optional<std::string> object_key = referencedObject(referenceKey(path, args));
  if (!object_key)
    return nullptr;
  std::optional<std::string> encoded = readContent(objectPath(*object_key));
  if (!encoded || digest(*encoded) != *object_key)
    return nullptr;
  auto entry = decodeEntry(*encoded);
  StringRef current_root = workspace ? StringRef(workspace->root) : StringRef();
  if (!entry || entry->format != g_config->cache.format || !validateEntry(*entry, path, current_root))
    return nullptr;
  auto file = deserialize(entry->format, path, entry->index.str(), entry->content.str(), IndexFile::kMajorVersion);
  if (file)
    rebaseIndex(*file, entry->origin, current_root);
  return file;
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

  std::optional<WorkspaceIdentity> workspace = workspaceFor(path, file.args);
  std::string reference_path = referencePath(referenceKey(path, file.args));
  std::string content_reference_path = referencePath(contentReferenceKey(path, file.args));
  if (deleted) {
    (void)sys::fs::remove(reference_path);
    (void)sys::fs::remove(content_reference_path);
    return;
  }
  std::string encoded = encodeEntry(file, workspace ? StringRef(workspace->root) : StringRef());
  if (encoded.empty())
    return;
  std::string object_key = digest(encoded);
  std::string object_path = objectPath(object_key);
  std::optional<std::string> existing = readContent(object_path);
  if ((!existing || digest(*existing) != object_key) && !writeAtomically(object_path, encoded))
    return;
  if (writeAtomically(reference_path, object_key + '\n'))
    (void)writeAtomically(content_reference_path, object_key + '\n');
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
  static const std::vector<const char *> no_args;
  std::optional<std::string> object_key = referencedObject(contentReferenceKey(path, no_args));
  if (!object_key)
    return {};
  std::optional<std::string> encoded = readContent(objectPath(*object_key));
  if (!encoded || digest(*encoded) != *object_key)
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
