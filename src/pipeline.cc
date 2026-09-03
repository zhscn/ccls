// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "pipeline.hh"

#include "cache.hh"
#include "config.hh"
#include "log.hh"
#include "lsp.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "platform.hh"
#include "project.hh"
#include "query.hh"
#include "sema_manager.hh"

#include <llvm/Support/JSON.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>
#include <llvm/Support/Threading.h>
#include <llvm/Support/raw_ostream.h>

#include <chrono>
#include <inttypes.h>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif
using namespace llvm;
namespace chrono = std::chrono;

namespace ccls {
namespace {
struct PublishDiagnosticParam {
  DocumentUri uri;
  std::vector<Diagnostic> diagnostics;
};
REFLECT_STRUCT(PublishDiagnosticParam, uri, diagnostics);

constexpr char index_progress_token[] = "index";
struct WorkDoneProgressCreateParam {
  const char *token = index_progress_token;
};
REFLECT_STRUCT(WorkDoneProgressCreateParam, token);
} // namespace

void VFS::clear() {
  std::lock_guard lock(mutex);
  state.clear();
}

int VFS::loaded(const std::string &path) {
  std::lock_guard lock(mutex);
  return state[path].loaded;
}

bool VFS::stamp(const std::string &path, int64_t ts, int step) {
  std::lock_guard<std::mutex> lock(mutex);
  State &st = state[path];
  if (st.timestamp < ts || (st.timestamp == ts && st.step < step)) {
    st.timestamp = ts;
    st.step = step;
    return true;
  } else
    return false;
}

struct MessageHandler;
void standaloneInitialize(MessageHandler &, const std::string &root);

namespace pipeline {

std::atomic<bool> g_quit;
std::atomic<int64_t> loaded_ts{0}, request_id{0};
IndexStats stats;
int64_t tick = 0;

namespace {

struct IndexRequest {
  std::string path;
  std::vector<const char *> args;
  IndexMode mode;
  bool must_exist = false;
  RequestId id;
  int64_t ts = tick++;
  int prio = 0; // For didOpen sorting
};

std::mutex thread_mtx;
std::condition_variable no_active_threads;
int active_threads;

MultiQueueWaiter *main_waiter;
MultiQueueWaiter *indexer_waiter;
MultiQueueWaiter *stdout_waiter;
ThreadedQueue<InMessage> *on_request;
ThreadedQueue<IndexRequest> *index_request;
ThreadedQueue<IndexUpdate> *on_indexed;
ThreadedQueue<std::string> *for_stdout;

bool cacheInvalid(VFS *vfs, IndexFile *prev, const std::string &path, const std::vector<const char *> &args,
                  const std::optional<std::string> &from) {
  {
    std::lock_guard<std::mutex> lock(vfs->mutex);
    if (prev->mtime < vfs->state[path].timestamp) {
      LOG_V(1) << "timestamp changed for " << path << (from ? " (via " + *from + ")" : std::string());
      return true;
    }
  }

  // For inferred files, allow -o a a.cc -> -o b b.cc
  StringRef stem = sys::path::stem(path);
  int changed = -1, size = std::min(prev->args.size(), args.size());
  for (int i = 0; i < size; i++)
    if (strcmp(prev->args[i], args[i]) && sys::path::stem(args[i]) != stem) {
      changed = i;
      break;
    }
  if (changed < 0 && prev->args.size() != args.size())
    changed = size;
  if (changed >= 0)
    LOG_V(1) << "args changed for " << path << (from ? " (via " + *from + ")" : std::string())
             << "; old: " << (changed < prev->args.size() ? prev->args[changed] : "")
             << "; new: " << (changed < size ? args[changed] : "");
  return changed >= 0;
};

bool indexer_Parse(SemaManager * /*completion*/, WorkingFiles *wfiles, Project *project, VFS *vfs,
                   const GroupMatch &matcher) {
  std::optional<IndexRequest> opt_request = index_request->tryPopFront();
  if (!opt_request)
    return false;
  auto &request = *opt_request;
  bool loud = request.mode != IndexMode::OnChange;

  // Dummy one to trigger refresh semantic highlight.
  if (request.path.empty()) {
    IndexUpdate dummy;
    dummy.refresh = true;
    on_indexed->pushBack(std::move(dummy), false);
    return false;
  }

  struct RAII {
    ~RAII() { stats.completed++; }
  } raii;
  if (!matcher.matches(request.path)) {
    LOG_IF_S(INFO, loud) << "skip " << request.path;
    return false;
  }

  Project::Entry entry = project->findEntry(request.path, true, request.must_exist);
  if (request.must_exist && entry.filename.empty())
    return true;
  if (request.args.size())
    entry.args = request.args;
  std::string path_to_index = entry.filename;
  std::unique_ptr<IndexFile> prev;

  bool deleted = request.mode == IndexMode::Delete,
       no_linkage = g_config->index.initialNoLinkage || request.mode != IndexMode::Background;
  int reparse = 0;
  if (deleted)
    reparse = 2;
  else if (!(g_config->index.onChange && wfiles->getFile(path_to_index))) {
    std::optional<int64_t> write_time = lastWriteTime(path_to_index);
    if (!write_time) {
      deleted = true;
    } else {
      if (vfs->stamp(path_to_index, *write_time, no_linkage ? 2 : 0))
        reparse = 1;
      if (request.path != path_to_index) {
        std::optional<int64_t> mtime1 = lastWriteTime(request.path);
        if (!mtime1)
          deleted = true;
        else if (vfs->stamp(request.path, *mtime1, no_linkage ? 2 : 0))
          reparse = 2;
      }
    }
  }

  if (g_config->index.onChange) {
    reparse = 2;
    std::lock_guard lock(vfs->mutex);
    vfs->state[path_to_index].step = 0;
    if (request.path != path_to_index)
      vfs->state[request.path].step = 0;
  }
  bool track = g_config->index.trackDependency > 1 || (g_config->index.trackDependency == 1 && request.ts < loaded_ts);
  if (!reparse && !track)
    return true;

  if (reparse < 2)
    do {
      std::unique_lock lock(indexCache().mutex(path_to_index));
      prev = indexCache().load(path_to_index);
      if (!prev || prev->no_linkage < no_linkage ||
          cacheInvalid(vfs, prev.get(), path_to_index, entry.args, std::nullopt))
        break;
      if (track)
        for (const auto &dep : prev->dependencies) {
          if (auto mtime1 = lastWriteTime(dep.first.val().str())) {
            if (dep.second < *mtime1) {
              reparse = 2;
              LOG_V(1) << "timestamp changed for " << path_to_index << " via " << dep.first.val().str();
              break;
            }
          } else {
            reparse = 2;
            LOG_V(1) << "timestamp changed for " << path_to_index << " via " << dep.first.val().str();
            break;
          }
        }
      if (reparse == 0)
        return true;
      if (reparse == 2)
        break;

      if (vfs->loaded(path_to_index))
        return true;
      LOG_S(INFO) << "load cache for " << path_to_index;
      auto dependencies = prev->dependencies;
      IndexUpdate update = IndexUpdate::createDelta(nullptr, prev.get());
      on_indexed->pushBack(std::move(update), request.mode != IndexMode::Background);
      {
        std::lock_guard lock1(vfs->mutex);
        VFS::State &st = vfs->state[path_to_index];
        st.loaded++;
        if (prev->no_linkage)
          st.step = 2;
      }
      lock.unlock();

      for (const auto &dep : dependencies) {
        std::string path = dep.first.val().str();
        if (!vfs->stamp(path, dep.second, 1))
          continue;
        std::lock_guard lock1(indexCache().mutex(path));
        prev = indexCache().load(path);
        if (!prev)
          continue;
        {
          std::lock_guard lock2(vfs->mutex);
          VFS::State &st = vfs->state[path];
          if (st.loaded)
            continue;
          st.loaded++;
          st.timestamp = prev->mtime;
          if (prev->no_linkage)
            st.step = 3;
        }
        IndexUpdate update = IndexUpdate::createDelta(nullptr, prev.get());
        on_indexed->pushBack(std::move(update), request.mode != IndexMode::Background);
        if (entry.id >= 0) {
          std::lock_guard lock2(project->mtx);
          project->root2folder[entry.root].path2entry_index[path] = entry.id;
        }
      }
      return true;
    } while (0);

  std::vector<std::unique_ptr<IndexFile>> indexes;
  int n_errs = 0;
  std::string first_error;
  if (deleted) {
    indexes.push_back(std::make_unique<IndexFile>(request.path, "", false));
    if (request.path != path_to_index)
      indexes.push_back(std::make_unique<IndexFile>(path_to_index, "", false));
  } else {
    std::vector<std::pair<std::string, std::string>> remapped;
    if (g_config->index.onChange) {
      std::string content = wfiles->getContent(path_to_index);
      if (content.size())
        remapped.emplace_back(path_to_index, content);
    }
    bool ok;
    auto result = idx::index(wfiles, vfs, entry.directory, path_to_index, entry.args, remapped, no_linkage, ok);
    indexes = std::move(result.indexes);
    n_errs = result.n_errs;
    first_error = std::move(result.first_error);

    if (!ok) {
      if (request.id.valid()) {
        ResponseError err;
        err.code = ErrorCode::InternalError;
        err.message = "failed to index " + path_to_index;
        pipeline::replyError(request.id, err);
      }
      return true;
    }
  }

  if (loud || n_errs) {
    std::string line;
    SmallString<64> tmp;
    SmallString<256> msg;
    (Twine(deleted ? "delete " : "parse ") + path_to_index).toVector(msg);
    if (n_errs)
      msg += " error:" + std::to_string(n_errs) + ' ' + first_error;
    if (LOG_V_ENABLED(1)) {
      msg += "\n ";
      for (const char *arg : entry.args)
        (msg += ' ') += arg;
    }
    LOG_S(INFO) << std::string_view(msg.data(), msg.size());
  }

  for (std::unique_ptr<IndexFile> &curr : indexes) {
    std::string path = curr->path;
    if (!matcher.matches(path)) {
      LOG_IF_S(INFO, loud) << "skip index for " << path;
      continue;
    }

    if (!deleted)
      LOG_IF_S(INFO, loud) << "store index for " << path << " (delta: " << !!prev << ")";
    {
      std::lock_guard lock(indexCache().mutex(path));
      int loaded = vfs->loaded(path);
      if (loaded)
        prev = indexCache().load(path);
      else
        prev.reset();
      indexCache().store(*curr, loaded, deleted);
      on_indexed->pushBack(IndexUpdate::createDelta(prev.get(), curr.get()), request.mode != IndexMode::Background);
      {
        std::lock_guard lock1(vfs->mutex);
        vfs->state[path].loaded++;
      }
      if (entry.id >= 0) {
        std::lock_guard lock(project->mtx);
        auto &folder = project->root2folder[entry.root];
        for (auto &dep : curr->dependencies)
          folder.path2entry_index[dep.first.val().str()] = entry.id;
      }
    }
  }

  return true;
}

void quit(SemaManager &manager) {
  g_quit.store(true, std::memory_order_relaxed);
  manager.quit();

  {
    std::lock_guard lock(index_request->mutex_);
  }
  indexer_waiter->cv.notify_all();
  {
    std::lock_guard lock(for_stdout->mutex_);
  }
  stdout_waiter->cv.notify_one();
  std::unique_lock lock(thread_mtx);
  no_active_threads.wait(lock, [] { return !active_threads; });
}

} // namespace

void threadEnter() {
  std::lock_guard lock(thread_mtx);
  active_threads++;
}

void threadLeave() {
  std::lock_guard lock(thread_mtx);
  if (!--active_threads)
    no_active_threads.notify_one();
}

void init() {
  main_waiter = new MultiQueueWaiter;
  on_request = new ThreadedQueue<InMessage>(main_waiter);
  on_indexed = new ThreadedQueue<IndexUpdate>(main_waiter);

  indexer_waiter = new MultiQueueWaiter;
  index_request = new ThreadedQueue<IndexRequest>(indexer_waiter);

  stdout_waiter = new MultiQueueWaiter;
  for_stdout = new ThreadedQueue<std::string>(stdout_waiter);
}

void indexer_Main(SemaManager *manager, VFS *vfs, Project *project, WorkingFiles *wfiles) {
  GroupMatch matcher(g_config->index.whitelist, g_config->index.blacklist);
  while (true)
    if (!indexer_Parse(manager, wfiles, project, vfs, matcher))
      if (indexer_waiter->wait(g_quit, index_request))
        break;
}

void indexerSort(const std::unordered_map<std::string, int> &dir2prio) {
  index_request->apply([&](std::deque<IndexRequest> &q) {
    for (IndexRequest &request : q) {
      std::string cur = lowerPathIfInsensitive(request.path);
      while (!(cur = llvm::sys::path::parent_path(cur)).empty()) {
        auto it = dir2prio.find(cur);
        if (it != dir2prio.end()) {
          request.prio = it->second;
          LOG_V(3) << "set priority " << request.prio << " to " << request.path;
          break;
        }
      }
    }
    std::stable_sort(q.begin(), q.end(), [](auto &l, auto &r) { return l.prio > r.prio; });
  });
}

void main_OnIndexed(DB *db, WorkingFiles *wfiles, IndexUpdate *update) {
  if (update->refresh) {
    LOG_S(INFO) << "loaded project. Refresh semantic highlight for all working file.";
    std::lock_guard lock(wfiles->mutex);
    for (auto &[f, wf] : wfiles->files) {
      std::string path = lowerPathIfInsensitive(f);
      if (db->name2file_id.find(path) == db->name2file_id.end())
        continue;
      QueryFile &file = db->files[db->name2file_id[path]];
      emitSemanticHighlight(db, wf.get(), file);
    }
    if (g_config->client.semanticTokensRefresh) {
      std::optional<bool> param;
      request("workspace/semanticTokens/refresh", param);
    }
    return;
  }

  db->applyIndexUpdate(update);

  // Update indexed content, skipped ranges, and semantic highlighting.
  if (update->files_def_update) {
    auto &def_u = *update->files_def_update;
    if (WorkingFile *wfile = wfiles->getFile(def_u.first.path)) {
      // FIXME With index.onChange: true, use buffer_content only for
      // request.path
      wfile->setIndexContent(g_config->index.onChange ? wfile->buffer_content : def_u.second);
      QueryFile &file = db->files[update->file_id];
      emitSkippedRanges(wfile, file);
      emitSemanticHighlight(db, wfile, file);
      if (g_config->client.semanticTokensRefresh) {
        // Return filename, even if the spec indicates params is none.
        TextDocumentIdentifier param;
        param.uri = DocumentUri::fromPath(wfile->filename);
        request("workspace/semanticTokens/refresh", param);
      }
    }
  }
}

void launchStdin() {
  threadEnter();
  std::thread([]() {
    set_thread_name("stdin");
    std::string str;
    const std::string_view kContentLength("Content-Length: ");
    bool received_exit = false;
    while (true) {
      int len = 0;
      str.clear();
      while (true) {
        int c = getchar();
        if (c == EOF)
          goto quit;
        if (c == '\n') {
          if (str.empty())
            break;
          if (!str.compare(0, kContentLength.size(), kContentLength))
            len = atoi(str.c_str() + kContentLength.size());
          str.clear();
        } else if (c != '\r') {
          str += c;
        }
      }

      str.resize(len);
      for (int i = 0; i < len; ++i) {
        int c = getchar();
        if (c == EOF)
          goto quit;
        str[i] = c;
      }

      auto message = std::make_unique<char[]>(len);
      std::copy(str.begin(), str.end(), message.get());
      auto expected = llvm::json::parse(llvm::StringRef(message.get(), len));
      assert(expected && "Parse error");
      std::optional<llvm::json::Value> document = std::move(*expected);

      JsonReader reader{&*document};
      if (auto obj = document->getAsObject(); !obj || !obj->getString("jsonrpc") || *obj->getString("jsonrpc") != "2.0")
        break;
      RequestId id;
      std::string method;
      reflectMember(reader, "id", id);
      reflectMember(reader, "method", method);
      if (id.valid())
        LOG_V(2) << "receive RequestMessage: " << id.value << " " << method;
      else
        LOG_V(2) << "receive NotificationMessage " << method;
      if (method.empty())
        continue;
      received_exit = method == "exit";
      // g_config is not available before "initialize". Use 0 in that case.
      on_request->pushBack(
          {id, std::move(method), std::move(message), std::move(document),
           chrono::steady_clock::now() + chrono::milliseconds(g_config ? g_config->request.timeout : 0), ""});

      if (received_exit)
        break;
    }

  quit:
    if (!received_exit) {
      const llvm::StringRef str("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
      auto message = std::make_unique<char[]>(str.size());
      std::copy(str.begin(), str.end(), message.get());
      std::optional<llvm::json::Value> document = std::move(*llvm::json::parse(str));
      on_request->pushBack(
          {RequestId(), std::string("exit"), std::move(message), std::move(document), chrono::steady_clock::now(), ""});
    }
    threadLeave();
  }).detach();
}

void launchStdout() {
  threadEnter();
  std::thread([]() {
    set_thread_name("stdout");

    while (true) {
      std::vector<std::string> messages = for_stdout->dequeueAll();
      for (auto &s : messages) {
        llvm::outs() << "Content-Length: " << s.size() << "\r\n\r\n" << s;
        llvm::outs().flush();
      }
      if (stdout_waiter->wait(g_quit, for_stdout))
        break;
    }
    threadLeave();
  }).detach();
}

void mainLoop() {
  Project project;
  WorkingFiles wfiles;
  VFS vfs;

  SemaManager manager(
      &project, &wfiles,
      [](const std::string &path, std::vector<Diagnostic> diagnostics) {
        PublishDiagnosticParam params;
        params.uri = DocumentUri::fromPath(path);
        params.diagnostics = std::move(diagnostics);
        notify("textDocument/publishDiagnostics", params);
      },
      [](const RequestId &id) {
        if (id.valid()) {
          ResponseError err;
          err.code = ErrorCode::InternalError;
          err.message = "drop older completion request";
          replyError(id, err);
        }
      });

  DB db;

  // Setup shared references.
  MessageHandler handler;
  handler.db = &db;
  handler.project = &project;
  handler.vfs = &vfs;
  handler.wfiles = &wfiles;
  handler.manager = &manager;

  bool work_done_created = false, in_progress = false;
  bool has_indexed = false;
  int64_t last_completed = 0;
  std::deque<InMessage> backlog;
  StringMap<std::deque<InMessage *>> path2backlog;
  while (true) {
    if (backlog.size()) {
      auto now = chrono::steady_clock::now();
      handler.overdue = true;
      while (backlog.size()) {
        if (backlog[0].backlog_path.size()) {
          if (now < backlog[0].deadline)
            break;
          handler.run(backlog[0]);
          path2backlog[backlog[0].backlog_path].pop_front();
        }
        backlog.pop_front();
      }
      handler.overdue = false;
    }

    std::vector<InMessage> messages = on_request->dequeueAll();
    bool did_work = messages.size();
    for (InMessage &message : messages)
      try {
        handler.run(message);
      } catch (NotIndexed &ex) {
        backlog.push_back(std::move(message));
        backlog.back().backlog_path = ex.path;
        path2backlog[ex.path].push_back(&backlog.back());
      }

    // If the "exit" notification has been received, clear all index requests
    // to make indexers stop in time.
    if (g_quit.load(std::memory_order_relaxed)) {
      index_request->apply([&](std::deque<IndexRequest> &q) { q.clear(); });
    }

    bool indexed = false;
    for (int i = 20; i--;) {
      std::optional<IndexUpdate> update = on_indexed->tryPopFront();
      if (!update)
        break;
      did_work = true;
      indexed = true;
      main_OnIndexed(&db, &wfiles, &*update);
      if (update->files_def_update) {
        auto it = path2backlog.find(update->files_def_update->first.path);
        if (it != path2backlog.end()) {
          for (auto &message : it->second) {
            handler.run(*message);
            message->backlog_path.clear();
          }
          path2backlog.erase(it);
        }
      }
    }

    int64_t completed = stats.completed.load(std::memory_order_relaxed);
    if (completed != last_completed) {
      if (!work_done_created) {
        WorkDoneProgressCreateParam param;
        request("window/workDoneProgress/create", param);
        work_done_created = true;
      }

      int64_t enqueued = stats.enqueued.load(std::memory_order_relaxed);
      if (completed != enqueued) {
        if (!in_progress) {
          WorkDoneProgressParam param;
          param.token = index_progress_token;
          param.value.kind = "begin";
          param.value.title = "indexing";
          notify("$/progress", param);
          in_progress = true;
        }
        int64_t last_idle = stats.last_idle.load(std::memory_order_relaxed);
        WorkDoneProgressParam param;
        param.token = index_progress_token;
        param.value.kind = "report";
        param.value.message = (Twine(completed - last_idle) + "/" + Twine(enqueued - last_idle)).str();
        param.value.percentage = 100 * (completed - last_idle) / (enqueued - last_idle);
        notify("$/progress", param);
      } else if (in_progress) {
        stats.last_idle.store(enqueued, std::memory_order_relaxed);
        WorkDoneProgressParam param;
        param.token = index_progress_token;
        param.value.kind = "end";
        notify("$/progress", param);
        in_progress = false;
      }
      last_completed = completed;
    }

    if (did_work) {
      has_indexed |= indexed;
      if (g_quit.load(std::memory_order_relaxed))
        break;
    } else {
      if (has_indexed) {
        freeUnusedMemory();
        has_indexed = false;
      }
      if (backlog.empty())
        main_waiter->wait(g_quit, on_indexed, on_request);
      else
        main_waiter->waitUntil(backlog[0].deadline, on_indexed, on_request);
    }
  }

  quit(manager);
}

void standalone(const std::string &root) {
  Project project;
  WorkingFiles wfiles;
  VFS vfs;
  SemaManager manager(
      nullptr, nullptr, [](const std::string &, const std::vector<Diagnostic> &) {}, [](const RequestId &) {});

  MessageHandler handler;
  handler.project = &project;
  handler.wfiles = &wfiles;
  handler.vfs = &vfs;
  handler.manager = &manager;

  standaloneInitialize(handler, root);
  bool tty = sys::Process::StandardOutIsDisplayed();

  if (tty) {
    int entries = 0;
    for (auto &[_, folder] : project.root2folder)
      entries += folder.entries.size();
    printf("entries:   %4d\n", entries);
  }
  while (1) {
    (void)on_indexed->dequeueAll();
    int64_t enqueued = stats.enqueued, completed = stats.completed;
    if (tty) {
      printf("\rcompleted: %4" PRId64 "/%" PRId64, completed, enqueued);
      fflush(stdout);
    }
    if (completed == enqueued)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (tty)
    puts("");
  quit(manager);
}

void index(const std::string &path, const std::vector<const char *> &args, IndexMode mode, bool must_exist,
           RequestId id) {
  if (!path.empty())
    stats.enqueued++;
  index_request->pushBack({path, args, mode, must_exist, std::move(id)}, mode != IndexMode::Background);
}

void removeCache(const std::string &path) { indexCache().removeMemory(path); }

std::optional<std::string> loadIndexedContent(const std::string &path) { return indexCache().loadIndexedContent(path); }

void notifyOrRequest(const char *method, bool request, const std::function<void(JsonWriter &)> &fn) {
  std::string output;
  llvm::raw_string_ostream os(output);
  llvm::json::OStream w(os);
  w.objectBegin();
  w.attribute("jsonrpc", "2.0");
  w.attribute("method", method);
  if (request) {
    w.attribute("id", request_id.fetch_add(1, std::memory_order_relaxed));
  }
  w.attributeBegin("params");
  JsonWriter writer(&w);
  fn(writer);
  w.objectEnd();
  LOG_V(2) << (request ? "RequestMessage: " : "NotificationMessage: ") << method;
  for_stdout->pushBack(std::move(output));
}

static void reply(const RequestId &id, const char *key, const std::function<void(JsonWriter &)> &fn) {
  std::string output;
  llvm::raw_string_ostream os(output);
  llvm::json::OStream w(os);
  w.objectBegin();
  w.attribute("jsonrpc", "2.0");
  w.attributeBegin("id");
  switch (id.type) {
  case RequestId::kNone:
    w.value(nullptr);
    break;
  case RequestId::kInt:
    w.value(atoll(id.value.c_str()));
    break;
  case RequestId::kString:
    w.value(id.value);
    break;
  }
  w.attributeBegin(key);
  JsonWriter writer(&w);
  fn(writer);
  w.objectEnd();
  if (id.valid())
    LOG_V(2) << "respond to RequestMessage: " << id.value;
  for_stdout->pushBack(std::move(output));
}

void reply(const RequestId &id, const std::function<void(JsonWriter &)> &fn) { reply(id, "result", fn); }

void replyError(const RequestId &id, const std::function<void(JsonWriter &)> &fn) { reply(id, "error", fn); }
} // namespace pipeline
} // namespace ccls
