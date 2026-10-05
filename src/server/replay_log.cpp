#include "server/replay_log.hpp"

#include <zstd.h>

#include <cstring>
#include <stdexcept>

namespace tmk::server {

// ------------------------------------------------------------------------------------------------ writer

bool ReplayWriter::compressed_path(std::string_view path) { return path.size() >= 4 && path.substr(path.size() - 4) == ".zst"; }

ReplayWriter::ReplayWriter(const std::string& path, int level) {
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) throw std::runtime_error("cannot create recording " + path);
  if (compressed_path(path)) {
    cctx_ = ZSTD_createCCtx();
    if (!cctx_) {
      std::fclose(f_);
      throw std::runtime_error("zstd: out of memory");
    }
    ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(cctx_, ZSTD_c_checksumFlag, 1);  // a damaged frame is reported, not misread
    out_buf_.resize(ZSTD_CStreamOutSize());
  }
}

ReplayWriter::~ReplayWriter() {
  try {
    close();
  } catch (...) {  // a destructor must not throw; the recording is best effort once the run is over
  }
}

void ReplayWriter::write_out(const void* p, std::size_t n) {
  if (n && std::fwrite(p, 1, n, f_) != n) throw std::runtime_error("recording: write failed");
}

void ReplayWriter::compress(std::string_view data, bool end) {
  ZSTD_inBuffer in{data.data(), data.size(), 0};
  for (;;) {
    ZSTD_outBuffer out{out_buf_.data(), out_buf_.size(), 0};
    const std::size_t left = ZSTD_compressStream2(cctx_, &out, &in, end ? ZSTD_e_end : ZSTD_e_continue);
    if (ZSTD_isError(left)) throw std::runtime_error(std::string("zstd: ") + ZSTD_getErrorName(left));
    write_out(out_buf_.data(), out.pos);
    if (end ? left == 0 : in.pos == in.size) break;
  }
}

void ReplayWriter::write_line(std::string_view line) {
  if (!f_) return;
  in_ += line.size() + 1;
  if (!cctx_) {
    write_out(line.data(), line.size());
    write_out("\n", 1);
    return;
  }
  compress(line, false);
  compress("\n", false);
  frame_in_ += line.size() + 1;
  if (frame_in_ >= kFrameBytes) end_frame();
}

void ReplayWriter::end_frame() {
  if (!f_ || !cctx_ || frame_in_ == 0) return;
  compress({}, true);
  frame_in_ = 0;
  std::fflush(f_);
}

void ReplayWriter::close() {
  if (!f_) return;
  end_frame();
  if (cctx_) ZSTD_freeCCtx(cctx_);
  cctx_ = nullptr;
  std::FILE* f = f_;
  f_ = nullptr;
  if (std::fclose(f) != 0) throw std::runtime_error("recording: close failed");
}

// ------------------------------------------------------------------------------------------------ reader

ReplayReader::ReplayReader(const std::string& path) {
  f_ = std::fopen(path.c_str(), "rb");
  if (!f_) throw std::runtime_error("cannot open recording " + path);
  in_buf_.resize(ZSTD_DStreamInSize());
  in_len_ = std::fread(in_buf_.data(), 1, in_buf_.size(), f_);
  // zstd frame magic 0xFD2FB528, little endian.
  static constexpr unsigned char kMagic[4] = {0x28, 0xB5, 0x2F, 0xFD};
  if (in_len_ >= 4 && std::memcmp(in_buf_.data(), kMagic, 4) == 0) {
    dctx_ = ZSTD_createDCtx();
    if (!dctx_) {
      std::fclose(f_);
      throw std::runtime_error("zstd: out of memory");
    }
  }
}

ReplayReader::~ReplayReader() {
  if (dctx_) ZSTD_freeDCtx(dctx_);
  if (f_) std::fclose(f_);
}

bool ReplayReader::fill() {
  if (eof_) return false;
  // Drop consumed text so the buffer only holds the unfinished line.
  buf_.erase(0, pos_);
  pos_ = 0;
  // A full output buffer last time means zstd may still hold decoded data: drain it before reading more input.
  if (in_pos_ == in_len_ && !out_pending_) {
    in_len_ = std::fread(in_buf_.data(), 1, in_buf_.size(), f_);
    in_pos_ = 0;
    if (in_len_ == 0) {
      eof_ = true;
      return false;
    }
  }
  if (!dctx_) {
    buf_.append(in_buf_.data() + in_pos_, in_len_ - in_pos_);
    in_pos_ = in_len_;
    return true;
  }
  out_.resize(ZSTD_DStreamOutSize());
  ZSTD_inBuffer in{in_buf_.data(), in_len_, in_pos_};
  ZSTD_outBuffer o{out_.data(), out_.size(), 0};
  const std::size_t r = ZSTD_decompressStream(dctx_, &o, &in);
  if (ZSTD_isError(r)) throw std::runtime_error(std::string("recording: corrupt zstd data: ") + ZSTD_getErrorName(r));
  in_pos_ = in.pos;
  out_pending_ = o.pos == o.size;
  buf_.append(out_.data(), o.pos);
  return true;
}

bool ReplayReader::next_line(std::string& line) {
  for (;;) {
    const std::size_t nl = buf_.find('\n', pos_);
    if (nl != std::string::npos) {
      line.assign(buf_, pos_, nl - pos_);
      pos_ = nl + 1;
      return true;
    }
    if (!fill()) return false;  // what is left is an incomplete line (or frame) from an interrupted run
  }
}

}  // namespace tmk::server
