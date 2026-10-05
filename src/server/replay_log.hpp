#pragma once
// Replay log (design doc 09 §2, decision D45): the viewer protocol's JSON messages, one per line, each with a
// leading "t" (seconds since the recording started). Written plain (`.jsonl`) or as a sequence of independent
// zstd frames (`.zst`, about 10-20x smaller). Each frame holds whole lines and the board snapshot (the keyframe)
// is a frame of its own, so a run that is killed loses at most the open frame, and any standard tool reads the
// log (`zstd -dc run.jsonl.zst | jq`).
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

// zstd contexts (zstd.h stays out of this header).
struct ZSTD_CCtx_s;
struct ZSTD_DCtx_s;

namespace tmk::server {

class ReplayWriter {
 public:
  // Uncompressed bytes per zstd frame before it is closed: big enough for full compression ratio, small
  // enough that a crash loses little.
  static constexpr std::size_t kFrameBytes = 1u << 20;

  // Compresses when `path` ends in ".zst". Throws std::runtime_error if the file cannot be created.
  explicit ReplayWriter(const std::string& path, int level = 6);
  ~ReplayWriter();
  ReplayWriter(const ReplayWriter&) = delete;
  ReplayWriter& operator=(const ReplayWriter&) = delete;

  static bool compressed_path(std::string_view path);
  bool compressed() const { return cctx_ != nullptr; }
  void write_line(std::string_view line);  // appends '\n'
  void end_frame();                        // closes the current zstd frame (no-op when plain or empty)
  void close();                            // ends the last frame and closes the file; called by the destructor
  std::uint64_t bytes_in() const { return in_; }

 private:
  void write_out(const void* p, std::size_t n);
  void compress(std::string_view data, bool end);
  std::FILE* f_ = nullptr;
  ::ZSTD_CCtx_s* cctx_ = nullptr;
  std::string out_buf_;
  std::size_t frame_in_ = 0;  // uncompressed bytes in the open frame
  std::uint64_t in_ = 0;
};

class ReplayReader {
 public:
  // Detects zstd by its magic number, so a renamed file still reads. Throws if the file cannot be opened.
  explicit ReplayReader(const std::string& path);
  ~ReplayReader();
  ReplayReader(const ReplayReader&) = delete;
  ReplayReader& operator=(const ReplayReader&) = delete;

  bool compressed() const { return dctx_ != nullptr; }
  // Next complete line (without '\n'). A truncated tail (incomplete frame or line) is ignored. Throws on
  // corrupt compressed data.
  bool next_line(std::string& line);

 private:
  bool fill();  // decompress or read more into buf_; false at end of input
  std::FILE* f_ = nullptr;
  ::ZSTD_DCtx_s* dctx_ = nullptr;
  std::string in_buf_;
  std::size_t in_pos_ = 0, in_len_ = 0;
  std::string out_;  // decompression output scratch
  std::string buf_;  // decoded text not yet returned
  std::size_t pos_ = 0;
  bool out_pending_ = false;
  bool eof_ = false;
};

}  // namespace tmk::server
