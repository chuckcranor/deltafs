/*
 * Copyright (c) 2015-2019 Carnegie Mellon University and
 *         Los Alamos National Laboratory.
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#pragma once

#include "pdlfs-common/env.h"
#include "pdlfs-common/mutexlock.h"
#include "pdlfs-common/port.h"
#include "pdlfs-common/status.h"

#include <assert.h>
#include <stddef.h>
#include <deque>

namespace pdlfs {
namespace plfsio {

class DoubleBuffering {
 public:
  DoubleBuffering(port::Mutex* mu, port::CondVar* cv);

  // Append data into the buffer. Return OK on success, or a non-OK status on
  // errors. REQUIRES: __Finish() has NOT been called.
  template <typename T>
  Status __Add(const Slice& k, const Slice& v);

  // Force a buffer flush (compaction) and maybe wait for it.
  // Compaction does not force data to be sync'ed. Sync() does.
  // Return OK on success, or a non-OK status on errors.
  // REQUIRES: __Finish() has NOT been called.
  template <typename T>
  Status __Flush(bool wait);

  // Sync data to storage. By default, only data that is already scheduled
  // for compaction is sync'ed. Data that is in the write buffer and not yet
  // scheduled for compaction is not sync'ed, unless do_flush is set to true.
  // Will wait until all outstanding compactions are done before performing
  // the sync. Return OK on success, or a non-OK status on errors.
  // REQUIRES: __Finish() has NOT been called.
  template <typename T>
  Status __Sync(bool do_flush);

  // Wait until there is no outstanding compactions.
  // REQUIRES: __Finish() has NOT been called.
  Status __Wait();

  // Finalize the writes because all writes are done. All data in write buffer
  // will be scheduled for compaction and will be sync'ed to storage after
  // the compaction. Return OK on success, or a non-OK status on errors.
  // NOTE: No more write operations after this call.
  template <typename T>
  Status __Finish();

 protected:
  port::Mutex* mu_;
  port::CondVar* bg_cv_;

  template <typename T>
  Status Prepare(uint32_t* compac_seq, bool force = true,
                 const Slice& k = Slice(), const Slice& v = Slice());
  void WaitFor(uint32_t compac_seq);
  void WaitForCompactions();
  template <typename T>
  void TryScheduleCompaction(uint32_t*, void*);
  template <typename T>
  void DoCompaction(void*);

  // State below is protected by mu_
  uint32_t num_compac_scheduled_;
  uint32_t num_compac_completed_;
  bool finished_;  // If Finish() has been called
  uint32_t num_bg_compactions_;
  Status bg_status_;
  std::deque<void*> bufs_;
  void* membuf_;
};

#define __this static_cast<T*>(this)

// Finalize all writes and sync all remaining data in the write buffer to
// storage. Return OK on success, or a non-OK status on errors.
// REQUIRES: mu_ has been LOCKed.
template <typename T>
Status DoubleBuffering::__Finish() {
  mu_->AssertHeld();
  Status finish_status;
  if (finished_)  // __Finish() has already been called.
    return bg_status_;
  else {
    __Flush<T>(false);
  }

  // Wait until !has_bg_compaction_
  WaitForCompactions();
  if (bg_status_.ok()) {  // Sync and close
    bg_status_ = __this->SyncBackend(true /* close */);
    finish_status = bg_status_;
    bg_status_ =
        Status::AssertionFailed("Already finished", finish_status.ToString());
  } else {
    finish_status = bg_status_;
  }

  finished_ = true;
  return finish_status;
}

// Sync data so data hits storage, but will wait until all outstanding
// compactions are completed before performing the sync operation.
// Return OK on success, or a non-OK status on errors.
// REQUIRES: __Finish() has NOT been called.
// REQUIRES: mu_ has been LOCKed.
template <typename T>
Status DoubleBuffering::__Sync(bool flush) {
  mu_->AssertHeld();
  uint32_t my_compac_seq = 0;
  Status status;
  if (finished_)  // __Finish() has already been called
    status = bg_status_;
  else {
    status = Prepare<T>(&my_compac_seq, flush);
  }

  if (!status.ok()) {
    return status;
  } else {
    // If compaction is scheduled, wait for it until num_compac_completed_
    // >= my_compac_seq, otherwise my_compac_seq is 0 and
    // WaitFor(seq) will return immediately.
    WaitFor(my_compac_seq);
    // Then, wait until !has_bg_compaction_
    WaitForCompactions();
    if (bg_status_.ok()) {
      bg_status_ = __this->SyncBackend();
    }
    return bg_status_;
  }
}

// Force a compaction and maybe wait for it to complete.
// REQUIRES: __Finish() has NOT been called.
// REQUIRES: mu_ has been LOCKed.
template <typename T>
Status DoubleBuffering::__Flush(bool wait) {
  mu_->AssertHeld();
  uint32_t my_compac_seq = 0;
  Status status;
  if (finished_)  // __Finish() has already been called
    status = bg_status_;
  else {
    status = Prepare<T>(&my_compac_seq);
  }

  if (status.ok() && wait) {  // Wait for compaction to clear
    WaitFor(my_compac_seq);
    return bg_status_;
  } else {
    return status;
  }
}

// Insert data into the buffer.
// Return OK on success, or a non-OK status on errors.
// REQUIRES: __Finish() has NOT been called.
// REQUIRES: mu_ has been LOCKed.
template <typename T>
Status DoubleBuffering::__Add(const Slice& k, const Slice& v) {
  mu_->AssertHeld();
  uint32_t ignored_compac_seq;
  Status status;
  if (finished_)  // __Finish() has already been called
    status = bg_status_;
  else {
    status = Prepare<T>(&ignored_compac_seq, false /* !force */, k, v);
    if (status.ok()) {
      assert(membuf_);
      __this->AddToBuffer(membuf_, k, v);
    }
  }

  return status;
}

// REQUIRES: mu_ has been LOCKed.
template <typename T>
Status DoubleBuffering::Prepare(uint32_t* seq, bool force, const Slice& k,
                                const Slice& v) {
  mu_->AssertHeld();
  Status status;
  while (true) {
    assert(membuf_);
    if (!bg_status_.ok()) {
      status = bg_status_;
      break;
    } else if (!force && __this->HasRoom(membuf_, k, v)) {
      // There is room in current write buffer
      break;
    } else if (bufs_.empty()) {
      bg_cv_->Wait();  // Wait for background compactions to finish
    } else {
      // Attempt to switch to a new write buffer
      force = false;
      TryScheduleCompaction<T>(seq, membuf_);
      membuf_ = bufs_.back();
      bufs_.pop_back();
    }
  }

  return status;
}

// REQUIRES: mu_ has been LOCKed.
template <typename T>
void DoubleBuffering::TryScheduleCompaction(uint32_t* compac_seq,
                                            void* immbuf) {
  mu_->AssertHeld();

  *compac_seq = ++num_compac_scheduled_;
  num_bg_compactions_++;

  if (__this->IsEmpty(immbuf)) {
    // Buffer is empty so compaction should be quick. As such we directly
    // execute the compaction in the current thread
    DoCompaction<T>(immbuf);  // No context switch guaranteed
  } else {
    __this->ScheduleCompaction(immbuf);
  }
}

// REQUIRES: mu_ has been LOCKed.
template <typename T>
void DoubleBuffering::DoCompaction(void* immbuf) {
  mu_->AssertHeld();
  assert(immbuf);
  Status status = __this->Compact(immbuf);
  ++num_compac_completed_;
  assert(bg_status_.ok());
  bg_status_ = status;
  __this->Clear(immbuf);
  bufs_.push_back(immbuf);
  assert(num_bg_compactions_ > 0);
  --num_bg_compactions_;
  // Just completed one compaction
  // Try launching another one
  uint32_t ignored_compac_seq;
  Prepare<T>(&ignored_compac_seq, false /* !force */);
  bg_cv_->SignalAll();
}

#undef __this

}  // namespace plfsio
}  // namespace pdlfs
