// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ccls {
struct IndexFile;

// Resolves the configured cache directory. "auto" selects a repository-wide
// directory under the user's cache home; relative paths remain project-local.
std::string resolveCacheDirectory(const std::string &project_root, const std::string &configured);

// Owns retained in-memory indexes and the disk cache's immutable,
// content-addressed objects and mutable references. The indexing pipeline
// deliberately knows nothing about cache paths or serialization layout.
class IndexCache {
public:
  IndexCache();
  ~IndexCache();

  IndexCache(const IndexCache &) = delete;
  IndexCache &operator=(const IndexCache &) = delete;

  // Serializes operations for one logical source path inside this process.
  std::mutex &mutex(const std::string &path);

  std::unique_ptr<IndexFile> load(const std::string &path, const std::vector<const char *> &args);
  void store(IndexFile &file, int previous_loads, bool deleted);

  // Drops the retained in-memory index while preserving its on-disk copy.
  void removeMemory(const std::string &path);
  std::optional<std::string> loadIndexedContent(const std::string &path);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

IndexCache &indexCache();
} // namespace ccls
