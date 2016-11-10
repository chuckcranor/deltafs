/*
 * Copyright (c) 2014-2016 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <set>

#include "pdlfs-common/mutexlock.h"

#include "mds_cli.h"

namespace pdlfs {

Status MDS::CLI::FetchIndex(const DirId& id, int zserver,
                            IndexHandle** result) {
  mutex_.AssertHeld();
  Status s;
  IndexHandle* h = index_cache_->Lookup(id);
  if (h == NULL) {
    mutex_.Unlock();

    DirIndex* idx = new DirIndex(&giga_);
    ReadidxOptions options;
    options.op_due = kMaxMicros;
    options.session_id = session_id_;
    options.dir_id = id;
    ReadidxRet ret;
    assert(zserver >= 0);
    size_t server = zserver % giga_.num_servers;
    s = factory_->Get(server)->Readidx(options, &ret);
    if (s.ok()) {
      if (!idx->Update(ret.idx) || idx->ZerothServer() != zserver) {
        s = Status::Corruption(Slice());
      }
    }

    mutex_.Lock();
    if (s.ok()) {
      h = index_cache_->Insert(id, idx);
    } else {
      delete idx;
    }
  }
  *result = h;
  return s;
}

Status MDS::CLI::Lookup(const DirId& pid, const Slice& name, int zserver,
                        uint64_t op_due, LookupHandle** result) {
  Status s;
  char tmp[20];
  Slice nhash = DirIndex::Hash(name, tmp);
  mutex_.AssertHeld();

  uint64_t now = Env::Default()->NowMicros();
  LookupHandle* h = lookup_cache_->Lookup(pid, nhash);

  // Ask for a new lookup state lease only if
  // we don't have one yet or
  // the one we current have has expired
  if (h == NULL || (now + 10) > lookup_cache_->Value(h)->LeaseDue()) {
    IndexHandle* idxh = NULL;
    s = FetchIndex(pid, zserver, &idxh);
    if (s.ok()) {
      assert(idxh != NULL);
      IndexGuard idxg(index_cache_, idxh);
      LookupOptions options;
      options.op_due = atomic_path_resolution_ ? op_due : kMaxMicros;
      options.session_id = session_id_;
      options.dir_id = pid;
      options.name_hash = nhash;
      if (paranoid_checks_) {
        options.name = name;
      }
      LookupRet ret;
      s = _Lookup(index_cache_->Value(idxh), options, &ret);
      if (s.ok()) {
        LookupStat* stat = new LookupStat(ret.stat);
        h = lookup_cache_->Insert(pid, nhash, stat);
        if (stat->LeaseDue() == 0) {
          lookup_cache_->Erase(pid, nhash);
        }
      }
    }
  }

  *result = h;
  return s;
}

Status MDS::CLI::_Lookup(const DirIndex* idx, const LookupOptions& options,
                         LookupRet* ret) {
  Status s;
  mutex_.AssertHeld();
  DirIndex* tmp_idx = NULL;
  assert(idx != NULL);
  const DirIndex* latest_idx = idx;
  int remaining_redirects = max_redirects_allowed_;
  mutex_.Unlock();

  do {
    try {
      assert(latest_idx != NULL);
      size_t server = latest_idx->HashToServer(options.name_hash);
      assert(server < giga_.num_servers);
      s = factory_->Get(server)->Lookup(options, ret);
    } catch (Redirect& re) {
      if (tmp_idx == NULL) {
        tmp_idx = new DirIndex(&giga_);
        tmp_idx->Update(*idx);
      }
      if (--remaining_redirects == 0 || !tmp_idx->Update(re)) {
        s = Status::Corruption("bad giga+ index");
      } else {
        s = Status::TryAgain(Slice());
      }
      assert(tmp_idx);
      latest_idx = tmp_idx;
    }
  } while (s.IsTryAgain());

  if (s.ok()) {
    if (paranoid_checks_) {
      if (!S_ISDIR(ret->stat.DirMode())) {
        s = Status::Corruption(Slice());
      }
    }
  }

  mutex_.Lock();
  if (tmp_idx != NULL) {
    if (s.ok()) {
      const DirId& pid = options.dir_id;
      IndexHandle* h = index_cache_->Insert(pid, tmp_idx);
      index_cache_->Release(h);
    } else {
      delete tmp_idx;
    }
  }

  return s;
}

bool MDS::CLI::IsReadDirOk(const PathInfo* info) {
  if (info == NULL) {
    return false;
  } else if (uid_ == 0) {
    return true;
  } else if (uid_ == info->uid && (info->mode & S_IRUSR) == S_IRUSR) {
    return true;
  } else if (gid_ == info->gid && (info->mode & S_IRGRP) == S_IRGRP) {
    return true;
  } else if ((info->mode & S_IROTH) == S_IROTH) {
    return true;
  } else {
    return false;
  }
}

bool MDS::CLI::IsWriteDirOk(const PathInfo* info) {
  if (info == NULL) {
    return false;
  } else if (uid_ == 0) {
    return true;
  } else if (uid_ == info->uid && (info->mode & S_IWUSR) == S_IWUSR) {
    return true;
  } else if (gid_ == info->gid && (info->mode & S_IWGRP) == S_IWGRP) {
    return true;
  } else if ((info->mode & S_IWOTH) == S_IWOTH) {
    return true;
  } else {
    return false;
  }
}

bool MDS::CLI::IsLookupOk(const PathInfo* info) {
  if (info == NULL) {
    return false;
  } else if (uid_ == 0) {
    return true;
  } else if (uid_ == info->uid && (info->mode & S_IXUSR) == S_IXUSR) {
    return true;
  } else if (gid_ == info->gid && (info->mode & S_IXGRP) == S_IXGRP) {
    return true;
  } else if ((info->mode & S_IXOTH) == S_IXOTH) {
    return true;
  } else {
    return false;
  }
}

Status MDS::CLI::ResolvePath(const Slice& path, PathInfo* result,
                             std::string* missing_parent) {
  mutex_.AssertHeld();

  Status s;
  Slice input(path);
  assert(input.size() != 0);
  assert(input[0] == '/');
  const static mode_t perm = ACCESSPERMS & ~S_IWOTH;
  const static mode_t mode = S_IFDIR | perm;
  result->lease_due = kMaxMicros;
  result->pid = DirId(0, 0, 0);
  result->zserver = 0;
  result->name = "/";
  result->depth = 0;
  result->mode = mode;
  result->uid = 0;
  result->gid = 0;

  input.remove_prefix(1);
  std::vector<PathInfo> parents(2, *result);
  uint64_t lease_due = result->lease_due;
  int depth = result->depth;
  if (!input.empty()) {
    // Start regular path resolution
    assert(!input.ends_with("/"));
    const char* p = strchr(input.c_str(), '/');
    for (; p != NULL; p = strchr(input.c_str(), '/')) {
      const char* q = input.c_str();
      input.remove_prefix(p - q + 1);
      Slice name = Slice(q, p - q);
      if (!name.empty()) {
        if (!IsLookupOk(result)) {
          s = Status::AccessDenied(Slice());
        } else if (name == "..") {
          assert(depth + 1 < parents.size());
          *result = parents[depth + 1];  // grand parent
          if (depth > 0) depth--;
        } else if (name == ".") {
          // Do nothing
        } else {
          depth++;
          result->name = name;
          parents.push_back(*result);
          LookupHandle* lh = NULL;
          s = Lookup(result->pid, name, result->zserver, lease_due, &lh);
          if (s.ok()) {
            assert(lh != NULL);
            const LookupStat* stat = lookup_cache_->Value(lh);
            assert(stat != NULL);
            lease_due = std::min(lease_due, stat->LeaseDue());

            result->pid = DirId(*stat);
            result->zserver = stat->ZerothServer();
            result->mode = stat->DirMode();
            result->uid = stat->UserId();
            result->gid = stat->GroupId();

            lookup_cache_->Release(lh);
          } else if (s.IsNotFound() && missing_parent != NULL) {
            const char* r = path.c_str();
            *missing_parent = std::string(r, p - r);
          }
        }

        if (!s.ok()) {
          break;
        }
      }
    }
  }

  if (s.ok()) {
    if (!input.empty()) {
      if (!IsLookupOk(result)) {
        s = Status::AccessDenied(Slice());
      } else if (input == "..") {
        assert(depth < parents.size());
        *result = parents[depth];  // grand grand parent
        if (depth > 0) depth--;
      } else if (input == ".") {
        assert(depth + 1 < parents.size());
        *result = parents[depth + 1];  // grand parent
      } else {
        result->name = input;
        depth++;
      }
    }

    if (s.ok()) {
      result->lease_due = lease_due;
      result->depth = depth;

#if VERBOSE >= 8
      Verbose(__LOG_ARGS__, 8, "ResolvePath '%s' -> pid=%s, name=%s, depth=%d",
              path.c_str(), result->pid.DebugString().c_str(),
              result->name.ToString().c_str(), result->depth);
#endif
    }
  }

  return s;
}

Status MDS::CLI::Fstat(const Slice& p, Fentry* ent) {
  Status s;
  char tmp[20];  // name hash buffer
  PathInfo path;
  MutexLock ml(&mutex_);
  s = ResolvePath(p, &path);
  if (s.ok()) {
    if (path.depth == 0) {
      s = Status::NotSupported("stating root directory");
    } else {
      IndexHandle* idxh = NULL;
      s = FetchIndex(path.pid, path.zserver, &idxh);
      if (s.ok()) {
        assert(idxh != NULL);
        IndexGuard idxg(index_cache_, idxh);
        FstatOptions options;
        options.op_due = atomic_path_resolution_ ? path.lease_due : kMaxMicros;
        options.session_id = session_id_;
        options.dir_id = path.pid;
        options.name_hash = DirIndex::Hash(path.name, tmp);
        if (paranoid_checks_) {
          options.name = path.name;
        }
        FstatRet ret;
        s = _Fstat(index_cache_->Value(idxh), options, &ret);
        if (s.ok()) {
          if (ent != NULL) {
            ent->pid = path.pid;
            ent->nhash = options.name_hash.ToString();
            ent->zserver = path.zserver;
            ent->stat = ret.stat;
          }
        }
      }
    }
  }

  return s;
}

Status MDS::CLI::_Fstat(const DirIndex* idx, const FstatOptions& options,
                        FstatRet* ret) {
  Status s;
  mutex_.AssertHeld();
  DirIndex* tmp_idx = NULL;
  assert(idx != NULL);
  const DirIndex* latest_idx = idx;
  int remaining_redirects = max_redirects_allowed_;
  mutex_.Unlock();

  do {
    try {
      assert(latest_idx != NULL);
      size_t server = latest_idx->HashToServer(options.name_hash);
      assert(server < giga_.num_servers);
      s = factory_->Get(server)->Fstat(options, ret);
    } catch (Redirect& re) {
      if (tmp_idx == NULL) {
        tmp_idx = new DirIndex(&giga_);
        tmp_idx->Update(*idx);
      }
      if (--remaining_redirects == 0 || !tmp_idx->Update(re)) {
        s = Status::Corruption("bad giga+ index");
      } else {
        s = Status::TryAgain(Slice());
      }
      assert(tmp_idx != NULL);
      latest_idx = tmp_idx;
    }
  } while (s.IsTryAgain());

  mutex_.Lock();
  if (tmp_idx != NULL) {
    if (s.ok()) {
      const DirId& pid = options.dir_id;
      IndexHandle* h = index_cache_->Insert(pid, tmp_idx);
      index_cache_->Release(h);
    } else {
      delete tmp_idx;
    }
  }

  return s;
}

Status MDS::CLI::Fcreat(const Slice& p, mode_t mode, Fentry* ent,
                        bool error_if_exists) {
  Status s;
  char tmp[20];  // name hash buffer
  PathInfo path;
  MutexLock ml(&mutex_);
  s = ResolvePath(p, &path);
  if (s.ok()) {
    if (path.depth == 0) {
      s = Status::AlreadyExists(Slice());
    } else if (!IsWriteDirOk(&path)) {
      s = Status::AccessDenied(Slice());
    } else if (path.name.size() > DELTAFS_NAME_MAX) {
      s = NameTooLong();
    } else {
      IndexHandle* idxh = NULL;
      s = FetchIndex(path.pid, path.zserver, &idxh);
      if (s.ok()) {
        assert(idxh != NULL);
        IndexGuard idxg(index_cache_, idxh);
        FcreatOptions options;
        options.op_due = atomic_path_resolution_ ? path.lease_due : kMaxMicros;
        options.session_id = session_id_;
        options.dir_id = path.pid;
        options.flags = error_if_exists ? O_EXCL : 0;
        options.mode = mode;
        options.uid = uid_;
        options.gid = gid_;
        options.name_hash = DirIndex::Hash(path.name, tmp);
        options.name = path.name;
        FcreatRet ret;
        s = _Fcreat(index_cache_->Value(idxh), options, &ret);
        if (s.ok()) {
          if (ent != NULL) {
            ent->pid = path.pid;
            ent->nhash = options.name_hash.ToString();
            ent->zserver = path.zserver;
            ent->stat = ret.stat;
          }
        }
      }
    }
  }

  return s;
}

Status MDS::CLI::_Fcreat(const DirIndex* idx, const FcreatOptions& options,
                         FcreatRet* ret) {
  Status s;
  mutex_.AssertHeld();
  DirIndex* tmp_idx = NULL;
  assert(idx != NULL);
  const DirIndex* latest_idx = idx;
  int remaining_redirects = max_redirects_allowed_;
  mutex_.Unlock();

  do {
    try {
      assert(latest_idx != NULL);
      size_t server = latest_idx->HashToServer(options.name_hash);
      assert(server < giga_.num_servers);
      s = factory_->Get(server)->Fcreat(options, ret);
    } catch (Redirect& re) {
      if (tmp_idx == NULL) {
        tmp_idx = new DirIndex(&giga_);
        tmp_idx->Update(*idx);
      }
      if (--remaining_redirects == 0 || !tmp_idx->Update(re)) {
        s = Status::Corruption("bad giga+ index");
      } else {
        s = Status::TryAgain(Slice());
      }
      assert(tmp_idx != NULL);
      latest_idx = tmp_idx;
    }
  } while (s.IsTryAgain());

  if (s.ok()) {
    if (paranoid_checks_) {
      if (!S_ISREG(ret->stat.FileMode())) {
        s = Status::Corruption(Slice());
      }
    }
  }

  mutex_.Lock();
  if (tmp_idx != NULL) {
    if (s.ok()) {
      const DirId& pid = options.dir_id;
      IndexHandle* h = index_cache_->Insert(pid, tmp_idx);
      index_cache_->Release(h);
    } else {
      delete tmp_idx;
    }
  }

  return s;
}

Status MDS::CLI::Unlink(const Slice& p, Fentry* ent, bool error_if_absent) {
  Status s;
  char tmp[20];  // name hash buffer
  PathInfo path;
  MutexLock ml(&mutex_);
  s = ResolvePath(p, &path);
  if (s.ok()) {
    if (path.depth == 0) {
      s = Status::NotSupported("deleting root directory");
    } else if (!IsWriteDirOk(&path)) {
      s = Status::AccessDenied(Slice());
    } else {
      IndexHandle* idxh = NULL;
      s = FetchIndex(path.pid, path.zserver, &idxh);
      if (s.ok()) {
        assert(idxh != NULL);
        IndexGuard idxg(index_cache_, idxh);
        UnlinkOptions options;
        options.op_due = atomic_path_resolution_ ? path.lease_due : kMaxMicros;
        options.session_id = session_id_;
        options.dir_id = path.pid;
        options.flags = error_if_absent ? O_EXCL : 0;
        options.name_hash = DirIndex::Hash(path.name, tmp);
        if (paranoid_checks_) {
          options.name = path.name;
        }
        UnlinkRet ret;
        s = _Unlink(index_cache_->Value(idxh), options, &ret);
        if (s.ok()) {
          if (ent != NULL) {
            ent->pid = path.pid;
            ent->nhash = options.name_hash.ToString();
            ent->zserver = path.zserver;
            ent->stat = ret.stat;
          }
        }
      }
    }
  }

  return s;
}

Status MDS::CLI::_Unlink(const DirIndex* idx, const UnlinkOptions& options,
                         UnlinkRet* ret) {
  Status s;
  mutex_.AssertHeld();
  DirIndex* tmp_idx = NULL;
  assert(idx != NULL);
  const DirIndex* latest_idx = idx;
  int remaining_redirects = max_redirects_allowed_;
  mutex_.Unlock();

  do {
    try {
      assert(latest_idx != NULL);
      size_t server = latest_idx->HashToServer(options.name_hash);
      assert(server < giga_.num_servers);
      s = factory_->Get(server)->Unlink(options, ret);
    } catch (Redirect& re) {
      if (tmp_idx == NULL) {
        tmp_idx = new DirIndex(&giga_);
        tmp_idx->Update(*idx);
      }
      if (--remaining_redirects == 0 || !tmp_idx->Update(re)) {
        s = Status::Corruption("bad giga+ index");
      } else {
        s = Status::TryAgain(Slice());
      }
      assert(tmp_idx != NULL);
      latest_idx = tmp_idx;
    }
  } while (s.IsTryAgain());

  mutex_.Lock();
  if (tmp_idx != NULL) {
    if (s.ok()) {
      const DirId& pid = options.dir_id;
      IndexHandle* h = index_cache_->Insert(pid, tmp_idx);
      index_cache_->Release(h);
    } else {
      delete tmp_idx;
    }
  }

  return s;
}

Status MDS::CLI::Mkdir(
    const Slice& p, mode_t mode, Fentry* ent,
    bool create_if_missing,  // auto create all missing ancestors
    bool error_if_exists) {
  Status s;
  char tmp[20];  // name hash buffer
  PathInfo path;
  std::string missing_parent;
  MutexLock ml(&mutex_);
  s = ResolvePath(p, &path, &missing_parent);
  if (s.IsNotFound() && create_if_missing) {
    if (!missing_parent.empty()) {
      mutex_.Unlock();
      s = Mkdir(missing_parent,
                mode & ~DELTAFS_DIR_MASK,  // avoid special directory modes
                NULL, true,  // recursively creating missing parents
                false        // okay if exists
                );
      if (s.ok()) {
        s = Mkdir(p, mode, ent, true,  // retry the original request
                  error_if_exists);
      }
      mutex_.Lock();
    }
  } else if (s.ok()) {
    if (path.depth == 0) {
      s = Status::AlreadyExists(Slice());
    } else if (!IsWriteDirOk(&path)) {
      s = Status::AccessDenied(Slice());
    } else if (path.name.size() > DELTAFS_NAME_MAX) {
      s = NameTooLong();
    } else {
      IndexHandle* idxh = NULL;
      s = FetchIndex(path.pid, path.zserver, &idxh);
      if (s.ok()) {
        assert(idxh != NULL);
        IndexGuard idxg(index_cache_, idxh);
        MkdirOptions options;
        options.op_due = atomic_path_resolution_ ? path.lease_due : kMaxMicros;
        options.session_id = session_id_;
        options.dir_id = path.pid;
        options.flags = error_if_exists ? O_EXCL : 0;
        options.mode = mode;
        options.uid = uid_;
        options.gid = gid_;
        options.name_hash = DirIndex::Hash(path.name, tmp);
        options.name = path.name;
        MkdirRet ret;
        s = _Mkdir(index_cache_->Value(idxh), options, &ret);
        if (s.ok()) {
          if (ent != NULL) {
            ent->pid = path.pid;
            ent->nhash = options.name_hash.ToString();
            ent->zserver = path.zserver;
            ent->stat = ret.stat;
          }
        }
      }
    }
  }

  return s;
}

Status MDS::CLI::_Mkdir(const DirIndex* idx, const MkdirOptions& options,
                        MkdirRet* ret) {
  Status s;
  mutex_.AssertHeld();
  DirIndex* tmp_idx = NULL;
  assert(idx != NULL);
  const DirIndex* latest_idx = idx;
  int remaining_redirects = max_redirects_allowed_;
  mutex_.Unlock();

  do {
    try {
      assert(latest_idx != NULL);
      size_t server = latest_idx->HashToServer(options.name_hash);
      assert(server < giga_.num_servers);
      s = factory_->Get(server)->Mkdir(options, ret);
    } catch (Redirect& re) {
      if (tmp_idx == NULL) {
        tmp_idx = new DirIndex(&giga_);
        tmp_idx->Update(*idx);
      }
      if (--remaining_redirects == 0 || !tmp_idx->Update(re)) {
        s = Status::Corruption("bad giga+ index");
      } else {
        s = Status::TryAgain(Slice());
      }
      assert(tmp_idx != NULL);
      latest_idx = tmp_idx;
    }
  } while (s.IsTryAgain());

  if (s.ok()) {
    if (paranoid_checks_) {
      if (!S_ISDIR(ret->stat.FileMode())) {
        s = Status::Corruption(Slice());
      }
    }
  }

  mutex_.Lock();
  if (tmp_idx != NULL) {
    if (s.ok()) {
      const DirId& pid = options.dir_id;
      IndexHandle* h = index_cache_->Insert(pid, tmp_idx);
      index_cache_->Release(h);
    } else {
      delete tmp_idx;
    }
  }

  return s;
}

Status MDS::CLI::Chmod(const Slice& p, mode_t mode, Fentry* ent) {
  Status s;
  char tmp[20];  // name hash buffer
  PathInfo path;
  MutexLock ml(&mutex_);
  s = ResolvePath(p, &path);
  if (s.ok()) {
    if (path.depth == 0) {
      s = Status::NotSupported("updating root directory");
    } else {
      IndexHandle* idxh = NULL;
      s = FetchIndex(path.pid, path.zserver, &idxh);
      if (s.ok()) {
        assert(idxh != NULL);
        IndexGuard idxg(index_cache_, idxh);
        ChmodOptions options;
        options.op_due = atomic_path_resolution_ ? path.lease_due : kMaxMicros;
        options.session_id = session_id_;
        options.dir_id = path.pid;
        options.mode = mode;
        options.name_hash = DirIndex::Hash(path.name, tmp);
        if (paranoid_checks_) {
          options.name = path.name;
        }
        ChmodRet ret;
        s = _Chmod(index_cache_->Value(idxh), options, &ret);
        if (s.ok()) {
          if (ent != NULL) {
            ent->pid = path.pid;
            ent->nhash = options.name_hash.ToString();
            ent->zserver = path.zserver;
            ent->stat = ret.stat;
          }
        }
      }
    }
  }

  return s;
}

Status MDS::CLI::_Chmod(const DirIndex* idx, const ChmodOptions& options,
                        ChmodRet* ret) {
  Status s;
  mutex_.AssertHeld();
  DirIndex* tmp_idx = NULL;
  assert(idx != NULL);
  const DirIndex* latest_idx = idx;
  int remaining_redirects = max_redirects_allowed_;
  mutex_.Unlock();

  do {
    try {
      assert(latest_idx != NULL);
      size_t server = latest_idx->HashToServer(options.name_hash);
      assert(server < giga_.num_servers);
      s = factory_->Get(server)->Chmod(options, ret);
    } catch (Redirect& re) {
      if (tmp_idx == NULL) {
        tmp_idx = new DirIndex(&giga_);
        tmp_idx->Update(*idx);
      }
      if (--remaining_redirects == 0 || !tmp_idx->Update(re)) {
        s = Status::Corruption("bad giga+ index");
      } else {
        s = Status::TryAgain(Slice());
      }
      assert(tmp_idx != NULL);
      latest_idx = tmp_idx;
    }
  } while (s.IsTryAgain());

  mutex_.Lock();
  if (tmp_idx != NULL) {
    if (s.ok()) {
      const DirId& pid = options.dir_id;
      IndexHandle* h = index_cache_->Insert(pid, tmp_idx);
      index_cache_->Release(h);
    } else {
      delete tmp_idx;
    }
  }

  return s;
}

// Send partial inode changes on an open file to metadata server.
// Cannot operate on directories.
// Metadata server identifies files through file paths. However, file
// path may change (due to rename, unlink, and creat operations)
// as long as we don't keep an active lease on the path that we use to
// open the file in the first place.
//
// Current solution is to always send file ids (reg_id + snap_id + ino)
// along with status updates. So the metadata server can detect
// conflicts either when the file we want to update no long exists
// (e.g. concurrently unlinked by others) or is no longer associated
// with the path (e.g. concurrently renamed by others).
Status MDS::CLI::Ftruncate(const Fentry& ent, uint64_t mtime, uint64_t size) {
  Status s;
  IndexHandle* idxh = NULL;
  MutexLock ml(&mutex_);
  s = FetchIndex(ent.pid, ent.zserver, &idxh);
  if (s.ok()) {
    mutex_.Unlock();

    assert(idxh != NULL);
    const DirIndex* idx = index_cache_->Value(idxh);
    assert(idx != NULL);
    TruncOptions options;  // TODO: add file id to options
    options.op_due = kMaxMicros;
    options.session_id = session_id_;
    options.dir_id = ent.pid;
    options.name_hash = ent.nhash;
    options.mtime = mtime;
    options.size = size;
    TruncRet ret;

    const DirIndex* latest_idx = idx;
    DirIndex* tmp_idx = NULL;
    int remaining_redirects = max_redirects_allowed_;
    do {
      try {
        assert(latest_idx != NULL);
        size_t server = latest_idx->HashToServer(ent.nhash);
        assert(server < giga_.num_servers);
        s = factory_->Get(server)->Trunc(options, &ret);
      } catch (Redirect& re) {
        if (tmp_idx == NULL) {
          tmp_idx = new DirIndex(&giga_);
          tmp_idx->Update(*idx);
        }
        if (--remaining_redirects == 0 || !tmp_idx->Update(re)) {
          s = Status::Corruption("bad giga+ index");
        } else {
          s = Status::TryAgain(Slice());
        }
        assert(tmp_idx != NULL);
        latest_idx = tmp_idx;
      }
    } while (s.IsTryAgain());
    if (s.ok()) {
      if (paranoid_checks_ && !S_ISREG(ret.stat.FileMode())) {
        s = Status::Corruption(Slice());
      }
    }

    mutex_.Lock();
    index_cache_->Release(idxh);
    if (tmp_idx != NULL) {
      if (s.ok()) {
        index_cache_->Release(index_cache_->Insert(ent.pid, tmp_idx));
      } else {
        delete tmp_idx;
      }
    }
  }

  return s;
}

Status MDS::CLI::Listdir(const Slice& p, std::vector<std::string>* names) {
  Status s;
  assert(!p.empty());
  if (p.size() != 1) assert(!p.ends_with("/"));
  std::string fake_path = p.ToString();
  fake_path += "/_";
  PathInfo path;
  s = ResolvePath(fake_path, &path);
  if (s.ok()) {
    if (!IsReadDirOk(&path)) {
      s = Status::AccessDenied(Slice());
    } else {
      IndexHandle* idxh = NULL;
      s = FetchIndex(path.pid, path.zserver, &idxh);
      if (s.ok()) {
        mutex_.Unlock();

        assert(idxh != NULL);
        const DirIndex* idx = index_cache_->Value(idxh);
        assert(idx != NULL);
        ListdirOptions options;
        options.op_due = atomic_path_resolution_ ? path.lease_due : kMaxMicros;
        options.session_id = session_id_;
        options.dir_id = path.pid;
        ListdirRet ret;
        ret.names = names;

        std::set<size_t> visited;
        int num_parts = 1 << idx->Radix();
        for (int i = 0; i < num_parts; i++) {
          if (idx->IsSet(i)) {
            size_t server = idx->GetServerForIndex(i);
            assert(server < giga_.num_servers);
            if (visited.count(server) == 0) {
              factory_->Get(server)->Listdir(options, &ret);
              visited.insert(server);
              if (visited.size() >= giga_.num_servers) {
                break;
              }
            }
          }
        }

        mutex_.Lock();
        index_cache_->Release(idxh);
      }
    }
  }
  return s;
}

Status MDS::CLI::Accessdir(const Slice& p, int mode) {
  Status s;
  assert(!p.empty());
  if (p.size() != 1) assert(!p.ends_with("/"));
  std::string fake_path = p.ToString();
  fake_path += "/_";
  PathInfo path;
  static const bool kPrefetchDirIndex = false;
  s = ResolvePath(fake_path, &path);
  if (s.ok()) {
    if ((mode & R_OK) == R_OK && !IsReadDirOk(&path)) {
      s = Status::AccessDenied(Slice());
    } else if ((mode & W_OK) == W_OK && !IsWriteDirOk(&path)) {
      s = Status::AccessDenied(Slice());
    } else if ((mode & X_OK) == X_OK && !IsLookupOk(&path)) {
      s = Status::AccessDenied(Slice());
    } else if (kPrefetchDirIndex) {
      IndexHandle* idxh = NULL;
      s = FetchIndex(path.pid, path.zserver, &idxh);
      if (s.ok()) {
        assert(idxh != NULL);
        index_cache_->Release(idxh);
      }
    }
  }
  return s;
}

bool MDS::CLI::IsReadOk(const Stat* s) {
  if (s == NULL) {
    return false;
  } else if (uid_ == 0) {
    return true;
  } else if (uid_ == s->UserId() && (s->FileMode() & S_IRUSR) == S_IRUSR) {
    return true;
  } else if (gid_ == s->GroupId() && (s->FileMode() & S_IRGRP) == S_IRGRP) {
    return true;
  } else if ((s->FileMode() & S_IROTH) == S_IROTH) {
    return true;
  } else {
    return false;
  }
}

bool MDS::CLI::IsWriteOk(const Stat* s) {
  if (s == NULL) {
    return false;
  } else if (uid_ == 0) {
    return true;
  } else if (uid_ == s->UserId() && (s->FileMode() & S_IWUSR) == S_IWUSR) {
    return true;
  } else if (gid_ == s->GroupId() && (s->FileMode() & S_IWGRP) == S_IWGRP) {
    return true;
  } else if ((s->FileMode() & S_IWOTH) == S_IWOTH) {
    return true;
  } else {
    return false;
  }
}

bool MDS::CLI::IsExecOk(const Stat* s) {
  if (s == NULL) {
    return false;
  } else if (uid_ == 0) {
    return true;
  } else if (uid_ == s->UserId() && (s->FileMode() & S_IXUSR) == S_IXUSR) {
    return true;
  } else if (gid_ == s->GroupId() && (s->FileMode() & S_IXGRP) == S_IXGRP) {
    return true;
  } else if ((s->FileMode() & S_IXOTH) == S_IXOTH) {
    return true;
  } else {
    return false;
  }
}

bool MDS::CLI::HasAccess(int acc_mode, const Stat* s) {
  if ((acc_mode & R_OK) == R_OK && !IsReadOk(s)) {
    return false;
  } else if ((acc_mode & W_OK) == W_OK && !IsWriteOk(s)) {
    return false;
  } else if ((acc_mode & X_OK) == X_OK && !IsExecOk(s)) {
    return false;
  } else {
    return true;
  }
}

Status MDS::CLI::Access(const Slice& p, int mode) {
  Status s;
  Fentry entry;
  s = Fstat(p, &entry);
  if (s.ok()) {
    if (!HasAccess(mode, &entry.stat)) {
      s = Status::AccessDenied(Slice());
    }
  }

  return s;
}

}  // namespace pdlfs
