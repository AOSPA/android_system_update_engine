//
// Copyright (C) 2012 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/payload_consumer/filesystem_verifier_action.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <base/bind.h>
#include <base/strings/string_util.h>
#include <brillo/data_encoding.h>
#include <brillo/message_loops/message_loop.h>
#include <brillo/secure_blob.h>
#include <brillo/streams/file_stream.h>

#include "payload_generator/delta_diff_generator.h"
#include "update_engine/common/utils.h"
#include "update_engine/payload_consumer/file_descriptor.h"

using brillo::data_encoding::Base64Encode;
using std::string;

// On a partition with verity enabled, we expect to see the following format:
// ===================================================
//              Normal Filesystem Data
// (this should take most of the space, like over 90%)
// ===================================================
//                  Hash tree
//         ~0.8% (e.g. 16M for 2GB image)
// ===================================================
//                  FEC data
//                    ~0.8%
// ===================================================
//                   Footer
//                     4K
// ===================================================

// For OTA that doesn't do on device verity computation, hash tree and fec data
// are written during DownloadAction as a regular InstallOp, so no special
// handling needed, we can just read the entire partition in 1 go.

// Verity enabled case: Only Normal FS data is written during download action.
// When hasing the entire partition, we will need to build the hash tree, write
// it to disk, then build FEC, and write it to disk. Therefore, it is important
// that we finish writing hash tree before we attempt to read & hash it. The
// same principal applies to FEC data.

// |verity_writer_| handles building and
// writing of FEC/HashTree, we just need to be careful when reading.
// Specifically, we must stop at beginning of Hash tree, let |verity_writer_|
// write both hash tree and FEC, then continue reading the remaining part of
// partition.

namespace chromeos_update_engine {

namespace {
const off_t kReadFileBufferSize = 128 * 1024;
}  // namespace

void FilesystemVerifierAction::PerformAction() {
  // Will tell the ActionProcessor we've failed if we return.
  ScopedActionCompleter abort_action_completer(processor_, this);

  if (!HasInputObject()) {
    LOG(ERROR) << "FilesystemVerifierAction missing input object.";
    return;
  }
  install_plan_ = GetInputObject();

  if (install_plan_.partitions.empty()) {
    LOG(INFO) << "No partitions to verify.";
    if (HasOutputPipe())
      SetOutputObject(install_plan_);
    abort_action_completer.set_code(ErrorCode::kSuccess);
    return;
  }
  install_plan_.Dump();
  StartPartitionHashing();
  abort_action_completer.set_should_complete(false);
}

void FilesystemVerifierAction::TerminateProcessing() {
  brillo::MessageLoop::current()->CancelTask(pending_task_id_);
  cancelled_ = true;
  Cleanup(ErrorCode::kSuccess);  // error code is ignored if canceled_ is true.
}

void FilesystemVerifierAction::Cleanup(ErrorCode code) {
  partition_fd_.reset();
  // This memory is not used anymore.
  buffer_.clear();

  // If we didn't write verity, partitions were maped. Releaase resource now.
  if (!install_plan_.write_verity &&
      dynamic_control_->UpdateUsesSnapshotCompression()) {
    LOG(INFO) << "Not writing verity and VABC is enabled, unmapping all "
                 "partitions";
    dynamic_control_->UnmapAllPartitions();
  }

  if (cancelled_)
    return;
  if (code == ErrorCode::kSuccess && HasOutputPipe())
    SetOutputObject(install_plan_);
  UpdateProgress(1.0);
  processor_->ActionComplete(this, code);
}

void FilesystemVerifierAction::UpdateProgress(double progress) {
  if (delegate_ != nullptr) {
    delegate_->OnVerifyProgressUpdate(progress);
  }
}

bool FilesystemVerifierAction::InitializeFdVABC() {
  const InstallPlan::Partition& partition =
      install_plan_.partitions[partition_index_];

  if (!ShouldWriteVerity()) {
    // In VABC, if we are not writing verity, just map all partitions,
    // and read using regular fd on |postinstall_mount_device| .
    // All read will go through snapuserd, which provides a consistent
    // view: device will use snapuserd to read partition during boot.
    // b/186196758
    // Call UnmapAllPartitions() first, because if we wrote verity before, these
    // writes won't be visible to previously opened snapuserd daemon. To ensure
    // that we will see the most up to date data from partitions, call Unmap()
    // then Map() to re-spin daemon.
    dynamic_control_->UnmapAllPartitions();
    dynamic_control_->MapAllPartitions();
    return InitializeFd(partition.readonly_target_path);
  }

  // FilesystemVerifierAction need the read_fd_.
  partition_fd_ =
      dynamic_control_->OpenCowFd(partition.name, partition.source_path, true);
  if (!partition_fd_) {
    LOG(ERROR) << "OpenCowReader(" << partition.name << ", "
               << partition.source_path << ") failed.";
    return false;
  }
  partition_size_ = partition.target_size;
  return true;
}

bool FilesystemVerifierAction::InitializeFd(const std::string& part_path) {
  partition_fd_ = FileDescriptorPtr(new EintrSafeFileDescriptor());
  const bool write_verity = ShouldWriteVerity();
  int flags = write_verity ? O_RDWR : O_RDONLY;
  if (!utils::SetBlockDeviceReadOnly(part_path, !write_verity)) {
    LOG(WARNING) << "Failed to set block device " << part_path << " as "
                 << (write_verity ? "writable" : "readonly");
  }
  if (!partition_fd_->Open(part_path.c_str(), flags)) {
    LOG(ERROR) << "Unable to open " << part_path << " for reading.";
    return false;
  }
  return true;
}

void FilesystemVerifierAction::StartPartitionHashing() {
  if (partition_index_ == install_plan_.partitions.size()) {
    if (!install_plan_.untouched_dynamic_partitions.empty()) {
      LOG(INFO) << "Verifying extents of untouched dynamic partitions ["
                << base::JoinString(install_plan_.untouched_dynamic_partitions,
                                    ", ")
                << "]";
      if (!dynamic_control_->VerifyExtentsForUntouchedPartitions(
              install_plan_.source_slot,
              install_plan_.target_slot,
              install_plan_.untouched_dynamic_partitions)) {
        Cleanup(ErrorCode::kFilesystemVerifierError);
        return;
      }
    }

    Cleanup(ErrorCode::kSuccess);
    return;
  }
  const InstallPlan::Partition& partition =
      install_plan_.partitions[partition_index_];
  string part_path;
  switch (verifier_step_) {
    case VerifierStep::kVerifySourceHash:
      part_path = partition.source_path;
      partition_size_ = partition.source_size;
      break;
    case VerifierStep::kVerifyTargetHash:
      part_path = partition.target_path;
      partition_size_ = partition.target_size;
      break;
  }

  LOG(INFO) << "Hashing partition " << partition_index_ << " ("
            << partition.name << ") on device " << part_path;
  auto success = false;
  if (dynamic_control_->UpdateUsesSnapshotCompression() &&
      verifier_step_ == VerifierStep::kVerifyTargetHash &&
      dynamic_control_->IsDynamicPartition(partition.name,
                                           install_plan_.target_slot)) {
    success = InitializeFdVABC();
  } else {
    if (part_path.empty()) {
      if (partition_size_ == 0) {
        LOG(INFO) << "Skip hashing partition " << partition_index_ << " ("
                  << partition.name << ") because size is 0.";
        partition_index_++;
        StartPartitionHashing();
        return;
      }
      LOG(ERROR) << "Cannot hash partition " << partition_index_ << " ("
                 << partition.name
                 << ") because its device path cannot be determined.";
      Cleanup(ErrorCode::kFilesystemVerifierError);
      return;
    }
    success = InitializeFd(part_path);
  }
  if (!success) {
    Cleanup(ErrorCode::kFilesystemVerifierError);
    return;
  }
  buffer_.resize(kReadFileBufferSize);
  hasher_ = std::make_unique<HashCalculator>();

  offset_ = 0;
  filesystem_data_end_ = partition_size_;
  CHECK_LE(partition.hash_tree_offset, partition.fec_offset)
      << " Hash tree is expected to come before FEC data";
  if (partition.hash_tree_offset != 0) {
    filesystem_data_end_ = partition.hash_tree_offset;
  } else if (partition.fec_offset != 0) {
    filesystem_data_end_ = partition.fec_offset;
  }
  if (ShouldWriteVerity()) {
    if (!verity_writer_->Init(partition)) {
      LOG(INFO) << "Verity writes enabled on partition " << partition.name;
      Cleanup(ErrorCode::kVerityCalculationError);
      return;
    }
  } else {
    LOG(INFO) << "Verity writes disabled on partition " << partition.name;
  }

  // Start the first read.
  ScheduleFileSystemRead();
}

bool FilesystemVerifierAction::ShouldWriteVerity() {
  const InstallPlan::Partition& partition =
      install_plan_.partitions[partition_index_];
  return verifier_step_ == VerifierStep::kVerifyTargetHash &&
         install_plan_.write_verity &&
         (partition.hash_tree_size > 0 || partition.fec_size > 0);
}

void FilesystemVerifierAction::ReadVerityAndFooter() {
  if (ShouldWriteVerity()) {
    if (!verity_writer_->Finalize(partition_fd_, partition_fd_)) {
      LOG(ERROR) << "Failed to write hashtree/FEC data.";
      Cleanup(ErrorCode::kFilesystemVerifierError);
      return;
    }
  }
  // Since we handed our |read_fd_| to verity_writer_ during |Finalize()|
  // call, fd's position could have been changed. Re-seek.
  partition_fd_->Seek(filesystem_data_end_, SEEK_SET);
  auto bytes_to_read = partition_size_ - filesystem_data_end_;
  while (bytes_to_read > 0) {
    const auto read_size = std::min<size_t>(buffer_.size(), bytes_to_read);
    auto bytes_read = partition_fd_->Read(buffer_.data(), read_size);
    if (bytes_read <= 0) {
      PLOG(ERROR) << "Failed to read hash tree " << bytes_read;
      Cleanup(ErrorCode::kFilesystemVerifierError);
      return;
    }
    if (!hasher_->Update(buffer_.data(), bytes_read)) {
      LOG(ERROR) << "Unable to update the hash.";
      Cleanup(ErrorCode::kError);
      return;
    }
    bytes_to_read -= bytes_read;
  }
  FinishPartitionHashing();
}

void FilesystemVerifierAction::ScheduleFileSystemRead() {
  // We can only start reading anything past |hash_tree_offset| after we have
  // already read all the data blocks that the hash tree covers. The same
  // applies to FEC.

  size_t bytes_to_read = std::min(static_cast<uint64_t>(buffer_.size()),
                                  filesystem_data_end_ - offset_);
  if (!bytes_to_read) {
    ReadVerityAndFooter();
    return;
  }
  partition_fd_->Seek(offset_, SEEK_SET);
  auto bytes_read = partition_fd_->Read(buffer_.data(), bytes_to_read);
  if (bytes_read < 0) {
    LOG(ERROR) << "Unable to schedule an asynchronous read from the stream. "
               << bytes_read;
    Cleanup(ErrorCode::kError);
  } else {
    // We could just invoke |OnReadDoneCallback()|, it works. But |PostTask|
    // is used so that users can cancel updates.
    pending_task_id_ = brillo::MessageLoop::current()->PostTask(
        base::Bind(&FilesystemVerifierAction::OnReadDone,
                   base::Unretained(this),
                   bytes_read));
  }
}

void FilesystemVerifierAction::OnReadDone(size_t bytes_read) {
  if (cancelled_) {
    Cleanup(ErrorCode::kError);
    return;
  }
  if (bytes_read == 0) {
    LOG(ERROR) << "Failed to read the remaining " << partition_size_ - offset_
               << " bytes from partition "
               << install_plan_.partitions[partition_index_].name;
    Cleanup(ErrorCode::kFilesystemVerifierError);
    return;
  }

  if (!hasher_->Update(buffer_.data(), bytes_read)) {
    LOG(ERROR) << "Unable to update the hash.";
    Cleanup(ErrorCode::kError);
    return;
  }

  // WE don't consider sizes of each partition. Every partition
  // has the same length on progress bar.
  // TODO(zhangkelvin) Take sizes of each partition into account

  UpdateProgress(
      (static_cast<double>(offset_) / partition_size_ + partition_index_) /
      install_plan_.partitions.size());
  if (ShouldWriteVerity()) {
    if (!verity_writer_->Update(offset_, buffer_.data(), bytes_read)) {
      LOG(ERROR) << "Unable to update verity";
      Cleanup(ErrorCode::kVerityCalculationError);
      return;
    }
  }

  offset_ += bytes_read;
  if (offset_ == filesystem_data_end_) {
    ReadVerityAndFooter();
    return;
  }

  ScheduleFileSystemRead();
}

void FilesystemVerifierAction::FinishPartitionHashing() {
  if (!hasher_->Finalize()) {
    LOG(ERROR) << "Unable to finalize the hash.";
    Cleanup(ErrorCode::kError);
    return;
  }
  InstallPlan::Partition& partition =
      install_plan_.partitions[partition_index_];
  LOG(INFO) << "Hash of " << partition.name << ": "
            << Base64Encode(hasher_->raw_hash());

  switch (verifier_step_) {
    case VerifierStep::kVerifyTargetHash:
      if (partition.target_hash != hasher_->raw_hash()) {
        LOG(ERROR) << "New '" << partition.name
                   << "' partition verification failed.";
        if (partition.source_hash.empty()) {
          // No need to verify source if it is a full payload.
          Cleanup(ErrorCode::kNewRootfsVerificationError);
          return;
        }
        // If we have not verified source partition yet, now that the target
        // partition does not match, and it's not a full payload, we need to
        // switch to kVerifySourceHash step to check if it's because the
        // source partition does not match either.
        verifier_step_ = VerifierStep::kVerifySourceHash;
      } else {
        partition_index_++;
      }
      break;
    case VerifierStep::kVerifySourceHash:
      if (partition.source_hash != hasher_->raw_hash()) {
        LOG(ERROR) << "Old '" << partition.name
                   << "' partition verification failed.";
        LOG(ERROR) << "This is a server-side error due to mismatched delta"
                   << " update image!";
        LOG(ERROR) << "The delta I've been given contains a " << partition.name
                   << " delta update that must be applied over a "
                   << partition.name << " with a specific checksum, but the "
                   << partition.name
                   << " we're starting with doesn't have that checksum! This"
                      " means that the delta I've been given doesn't match my"
                      " existing system. The "
                   << partition.name << " partition I have has hash: "
                   << Base64Encode(hasher_->raw_hash())
                   << " but the update expected me to have "
                   << Base64Encode(partition.source_hash) << " .";
        LOG(INFO) << "To get the checksum of the " << partition.name
                  << " partition run this command: dd if="
                  << partition.source_path
                  << " bs=1M count=" << partition.source_size
                  << " iflag=count_bytes 2>/dev/null | openssl dgst -sha256 "
                     "-binary | openssl base64";
        LOG(INFO) << "To get the checksum of partitions in a bin file, "
                  << "run: .../src/scripts/sha256_partitions.sh .../file.bin";
        Cleanup(ErrorCode::kDownloadStateInitializationError);
        return;
      }
      // The action will skip kVerifySourceHash step if target partition hash
      // matches, if we are in this step, it means target hash does not match,
      // and now that the source partition hash matches, we should set the
      // error code to reflect the error in target partition. We only need to
      // verify the source partition which the target hash does not match, the
      // rest of the partitions don't matter.
      Cleanup(ErrorCode::kNewRootfsVerificationError);
      return;
  }
  // Start hashing the next partition, if any.
  hasher_.reset();
  buffer_.clear();
  if (partition_fd_) {
    partition_fd_.reset();
  }
  StartPartitionHashing();
}

}  // namespace chromeos_update_engine
