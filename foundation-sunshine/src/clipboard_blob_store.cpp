/**
 * @file src/clipboard_blob_store.cpp
 * @brief See clipboard_blob_store.h.
 */
#include "clipboard_blob_store.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "src/crypto.h"
#include "src/platform/common.h"

namespace clipboard_blob_store {
  namespace {
    using clock_t = std::chrono::steady_clock;

    struct entry_t {
      payload_t bytes;
      std::string mime;
      std::filesystem::path file_path;
      std::uint64_t expected_size = 0;
      std::uint64_t written_bytes = 0;
      bool disk_backed = false;
      bool complete = true;
      clock_t::time_point expires_at;
    };

    std::mutex g_mu;
    std::mutex g_write_mu;
    // Map keyed by blob_id; insertion order tracked separately for FIFO eviction.
    std::unordered_map<blob_id, entry_t> g_entries;
    std::deque<blob_id> g_fifo;
    std::vector<std::filesystem::path> g_pending_deletes;
    std::size_t g_total_bytes = 0;
    std::uint64_t g_disk_total_bytes = 0;
    bool g_disk_dir_initialized = false;

    std::filesystem::path
    blob_dir() {
      return platf::appdata() / "clipboard_blobs";
    }

    void
    try_remove_file_locked(const std::filesystem::path &path) {
      if (path.empty()) {
        return;
      }
      std::error_code ec;
      if (std::filesystem::exists(path, ec) && !std::filesystem::remove(path, ec) && ec) {
        g_pending_deletes.push_back(path);
      }
    }

    void
    reap_pending_deletes_locked() {
      if (g_pending_deletes.empty()) {
        return;
      }
      g_pending_deletes.erase(
        std::remove_if(g_pending_deletes.begin(), g_pending_deletes.end(),
          [](const std::filesystem::path &path) {
            std::error_code ec;
            return !std::filesystem::exists(path, ec) || std::filesystem::remove(path, ec);
          }),
        g_pending_deletes.end());
    }

    bool
    ensure_blob_dir_locked() {
      const auto dir = blob_dir();
      std::error_code ec;
      if (!g_disk_dir_initialized) {
        g_disk_dir_initialized = true;
        if (std::filesystem::exists(dir, ec)) {
          for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            std::error_code rm_ec;
            std::filesystem::remove(entry.path(), rm_ec);
          }
        }
      }
      std::filesystem::create_directories(dir, ec);
      return !ec;
    }

    /// Generate a UUID-v4-shaped 36-char hex id using OpenSSL's CSPRNG.
    ///
    /// The id functions as a bearer capability: any HTTPS-authenticated client
    /// that learns it (via a KIND_REF wire frame) can fetch the blob bytes
    /// without further per-blob authorisation. To keep that property safe we
    /// must use a cryptographically-strong random source, not std::mt19937 —
    /// 122 bits of CSPRNG entropy + bounded TTL make guessing infeasible.
    std::string
    make_id() {
      const auto random = crypto::rand(16);
      if (random.size() != 16) {
        // A failed CSPRNG must not silently degrade this bearer capability.
        throw std::runtime_error("clipboard_blob_store: CSPRNG failed");
      }

      std::uint8_t raw[16];
      std::memcpy(raw, random.data(), sizeof(raw));

      // Force version=4 nibble and RFC 4122 variant bits.
      raw[6] = static_cast<std::uint8_t>((raw[6] & 0x0F) | 0x40);
      raw[8] = static_cast<std::uint8_t>((raw[8] & 0x3F) | 0x80);

      static constexpr char kHex[] = "0123456789abcdef";
      std::string s(36, '-');
      static constexpr int kSlots[] = { 0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34 };
      for (int i = 0; i < 16; ++i) {
        const std::size_t pos = static_cast<std::size_t>(kSlots[i]);
        s[pos] = kHex[raw[i] >> 4];
        s[pos + 1] = kHex[raw[i] & 0xF];
      }
      return s;
    }

    void
    erase_entry_locked(std::unordered_map<blob_id, entry_t>::iterator it) {
      if (it->second.disk_backed) {
        if (g_disk_total_bytes >= it->second.expected_size) {
          g_disk_total_bytes -= it->second.expected_size;
        }
        else {
          g_disk_total_bytes = 0;
        }
        try_remove_file_locked(it->second.file_path);
      }
      else {
        g_total_bytes -= it->second.bytes.size();
      }
      g_entries.erase(it);
    }

    /// Caller must hold g_mu. Drops everything past TTL.
    void
    sweep_locked(clock_t::time_point now) {
      reap_pending_deletes_locked();
      for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (it->second.expires_at <= now) {
          auto dead = it++;
          erase_entry_locked(dead);
        }
        else {
          ++it;
        }
      }
      while (!g_fifo.empty() && g_entries.find(g_fifo.front()) == g_entries.end()) {
        g_fifo.pop_front();
      }
    }

    /// Caller must hold g_mu. Force-evict oldest in-memory entries until total bytes
    /// + `incoming` fits under `kMaxStoreBytes`.
    void
    evict_for_locked(std::size_t incoming) {
      for (auto it = g_fifo.begin(); it != g_fifo.end() && g_total_bytes + incoming > kMaxStoreBytes;) {
        auto entry = g_entries.find(*it);
        if (entry == g_entries.end()) {
          it = g_fifo.erase(it);
          continue;
        }
        if (entry->second.disk_backed) {
          ++it;
          continue;
        }
        erase_entry_locked(entry);
        it = g_fifo.erase(it);
      }
    }

    /// Caller must hold g_mu. Force-evict oldest disk-backed entries until total disk bytes
    /// + `incoming` fits under `kMaxDiskStoreBytes`.
    void
    evict_disk_for_locked(std::uint64_t incoming) {
      for (auto it = g_fifo.begin(); it != g_fifo.end() && g_disk_total_bytes + incoming > kMaxDiskStoreBytes;) {
        auto entry = g_entries.find(*it);
        if (entry == g_entries.end()) {
          it = g_fifo.erase(it);
          continue;
        }
        if (!entry->second.disk_backed) {
          ++it;
          continue;
        }
        erase_entry_locked(entry);
        it = g_fifo.erase(it);
      }
    }
  }  // namespace

  put_result_t
  put(payload_t bytes, std::string mime) {
    if (bytes.size() > kMaxBlobBytes) {
      return { {}, false, "too_large" };
    }
    if (bytes.size() > kMaxStoreBytes) {
      // Even after a full FIFO purge it wouldn't fit.
      return { {}, false, "too_large" };
    }

    auto now = clock_t::now();
    const std::size_t incoming = bytes.size();

    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(now);
    evict_for_locked(incoming);

    blob_id id;
    try {
      id = make_id();
      // Defensive: ensure no collision (vanishingly unlikely).
      while (g_entries.find(id) != g_entries.end()) {
        id = make_id();
      }
    }
    catch (const std::exception &) {
      return { {}, false, "internal" };
    }

    entry_t e;
    e.bytes = std::move(bytes);
    e.mime = std::move(mime);
    e.expected_size = incoming;
    e.written_bytes = incoming;
    e.disk_backed = false;
    e.complete = true;
    e.expires_at = now + std::chrono::seconds(kBlobTtlSeconds);

    g_total_bytes += incoming;
    g_fifo.push_back(id);
    g_entries.emplace(id, std::move(e));

    return { id, true, {} };
  }

  init_result_t
  init_stream(std::uint64_t expected_size, std::string mime) {
    if (expected_size == 0 || expected_size > kMaxStreamBlobBytes || expected_size > kMaxDiskStoreBytes) {
      return { {}, false, "too_large" };
    }

    auto now = clock_t::now();
    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(now);
    evict_disk_for_locked(expected_size);

    if (!ensure_blob_dir_locked()) {
      return { {}, false, "internal" };
    }

    blob_id id;
    try {
      id = make_id();
      while (g_entries.find(id) != g_entries.end()) {
        id = make_id();
      }
    }
    catch (const std::exception &) {
      return { {}, false, "internal" };
    }

    const auto path = blob_dir() / (id + ".blob");
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        return { {}, false, "internal" };
      }
    }

    entry_t e;
    e.mime = std::move(mime);
    e.file_path = path;
    e.expected_size = expected_size;
    e.written_bytes = 0;
    e.disk_backed = true;
    e.complete = false;
    e.expires_at = now + std::chrono::seconds(kBlobTtlSeconds);

    g_disk_total_bytes += expected_size;
    g_fifo.push_back(id);
    g_entries.emplace(id, std::move(e));

    return { id, true, {} };
  }

  chunk_result_t
  append_chunk(const blob_id &id, std::string_view data) {
    if (data.empty()) {
      return { false, false, 0, "empty_body" };
    }
    if (data.size() > kMaxChunkBytes) {
      return { false, false, 0, "payload_too_large" };
    }

    std::lock_guard<std::mutex> write_lk(g_write_mu);
    auto now = clock_t::now();
    std::filesystem::path path;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      sweep_locked(now);
      auto it = g_entries.find(id);
      if (it == g_entries.end() || !it->second.disk_backed) {
        return { false, false, 0, "not_found" };
      }
      if (it->second.complete) {
        return { false, true, it->second.written_bytes, "already_complete" };
      }
      if (data.size() > it->second.expected_size - it->second.written_bytes) {
        return { false, false, it->second.written_bytes, "size_overflow" };
      }
      path = it->second.file_path;
      it->second.expires_at = now + std::chrono::seconds(kBlobTtlSeconds);
    }

    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open() || !out.write(data.data(), static_cast<std::streamsize>(data.size())) || !out.flush()) {
      return { false, false, 0, "write_failed" };
    }

    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_entries.find(id);
    if (it == g_entries.end() || !it->second.disk_backed) {
      return { false, false, 0, "not_found" };
    }
    it->second.written_bytes += data.size();
    it->second.expires_at = clock_t::now() + std::chrono::seconds(kBlobTtlSeconds);
    if (it->second.written_bytes >= it->second.expected_size) {
      it->second.complete = true;
    }
    return { true, it->second.complete, it->second.written_bytes, {} };
  }

  get_result_t
  get(const blob_id &id, bool consume) {
    auto now = clock_t::now();

    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(now);

    auto it = g_entries.find(id);
    if (it == g_entries.end() || !it->second.complete) {
      return { false, {}, {}, {} };
    }

    if (it->second.disk_backed) {
      it->second.expires_at = now + std::chrono::seconds(kBlobTtlSeconds);
      return { true, {}, it->second.mime, it->second.file_path };
    }

    if (consume) {
      get_result_t r { true, std::move(it->second.bytes), std::move(it->second.mime), {} };
      g_total_bytes -= r.bytes.size();
      g_entries.erase(it);
      // Lazy: leave the dead id in g_fifo; sweep_locked drops it on next pass.
      return r;
    }

    // Copy out without mutating storage so retries work.
    return { true, it->second.bytes, it->second.mime, {} };
  }

  void
  sweep_expired() {
    auto now = clock_t::now();
    std::lock_guard<std::mutex> lk(g_mu);
    sweep_locked(now);
  }

  stats_t
  stats() {
    std::lock_guard<std::mutex> lk(g_mu);
    return { g_entries.size(), g_total_bytes };
  }
}  // namespace clipboard_blob_store
