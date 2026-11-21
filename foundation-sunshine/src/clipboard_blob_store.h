/**
 * @file src/clipboard_blob_store.h
 * @brief In-memory TTL-bounded blob storage for out-of-band clipboard payloads
 *        (large images, files) that exceed the encrypted control-stream's
 *        single-frame limit (`clipboard_bridge::kMaxPayloadBytes`).
 *
 * Design:
 *   * Producer (GUI agent or HTTP upload from a client) puts opaque bytes +
 *     a MIME hint and gets back a freshly-minted UUID.
 *   * Consumers (other clients via HTTP, or the GUI agent dispatching an
 *     inbound large object) GET by id. Reads do NOT remove by default; the
 *     TTL sweeper reclaims memory eventually so a client may retry on
 *     transient network failures.
 *   * No cross-process persistence — restarting Sunshine drops everything.
 *     Acceptable: clipboard items are short-lived by nature, and the wire
 *     KIND_REF that points here is also delivered over a live session.
 *
 * Eviction policy:
 *   * Per-blob TTL (see `kBlobTtlSeconds`).
 *   * Hard cap on total resident bytes (`kMaxStoreBytes`); when a put would
 *     exceed it, oldest blobs are evicted FIFO until it fits, even if their
 *     TTL hasn't expired.
 *
 * Threading: all public functions are thread-safe.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "clipboard_bridge.h"  // for payload_t

namespace clipboard_blob_store {
  using payload_t = clipboard_bridge::payload_t;
  using blob_id = std::string;  ///< UUID v4 (canonical 36-char hex form).

  /// Per-blob hard size cap for single-shot in-memory uploads.
  constexpr std::size_t kMaxBlobBytes = 64ULL * 1024 * 1024;  // 64 MiB

  /// Per-blob hard size cap for chunked disk-backed file clipboard uploads.
  constexpr std::uint64_t kMaxStreamBlobBytes = 4ULL * 1024 * 1024 * 1024;  // 4 GiB

  /// Maximum size of a single chunk uploaded to a streamed blob.
  constexpr std::size_t kMaxChunkBytes = 16ULL * 1024 * 1024;  // 16 MiB

  /// Total resident bytes cap across all in-memory blobs. FIFO eviction kicks in when
  /// a put would exceed this, regardless of TTL.
  constexpr std::size_t kMaxStoreBytes = 256ULL * 1024 * 1024;  // 256 MiB

  /// Total bytes cap across all disk-backed streamed blobs.
  constexpr std::uint64_t kMaxDiskStoreBytes = 6ULL * 1024 * 1024 * 1024;  // 6 GiB

  /// Per-blob TTL after the most recent activity.
  constexpr int kBlobTtlSeconds = 600;

  struct put_result_t {
    blob_id id;       ///< Empty iff ok == false.
    bool ok;
    /// Reason for failure when !ok: "too_large" or "internal".
    std::string err;
  };

  /// Store bytes + MIME and return a freshly-minted id. The caller's vector
  /// is consumed. Triggers an opportunistic sweep + FIFO eviction.
  put_result_t put(payload_t bytes, std::string mime);

  struct init_result_t {
    blob_id id;
    bool ok;
    std::string err;
  };

  /// Allocate a disk-backed streaming blob for large file clipboard payloads.
  init_result_t init_stream(std::uint64_t expected_size, std::string mime);

  struct chunk_result_t {
    bool ok;
    bool complete;
    std::uint64_t written;
    std::string err;
  };

  /// Append a sequential chunk to a previously initialized streaming blob.
  chunk_result_t append_chunk(const blob_id &id, std::string_view data);

  struct get_result_t {
    bool found;
    payload_t bytes;
    std::string mime;
    std::filesystem::path file_path;
  };

  /// Look up by id. `found == false` for missing or expired entries.
  /// When `consume == true` the blob is removed on a successful read; useful
  /// for one-shot semantics where the consumer is sole.
  get_result_t get(const blob_id &id, bool consume = false);

  /// Force expiry of stale entries. Called opportunistically by put(); tests
  /// can also invoke it directly.
  void sweep_expired();

  /// Diagnostics — count and bytes of currently-resident entries.
  struct stats_t {
    std::size_t entries;
    std::size_t bytes;
  };

  stats_t stats();
}  // namespace clipboard_blob_store
