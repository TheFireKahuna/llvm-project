//===-- Implementation of open_wmemstream ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/stdio/open_wmemstream.h"

#include "hdr/errno_macros.h"
#include "hdr/func/realloc.h"
#include "hdr/stdio_macros.h"
#include "hdr/types/FILE.h"
#include "hdr/types/char32_t.h"
#include "hdr/types/char8_t.h"
#include "hdr/types/off_t.h"
#include "hdr/types/size_t.h"
#include "hdr/types/wchar_t.h"
#include "src/__support/CPP/new.h"
#include "src/__support/File/file.h"
#include "src/__support/File/file_io_result.h"
#include "src/__support/alloc-checker.h"
#include "src/__support/common.h"
#include "src/__support/error_or.h"
#include "src/__support/macros/config.h"
#include "src/__support/wchar/character_converter.h"
#include "src/__support/wchar/locale_encoding.h"
#include "src/__support/wchar/mbstate.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {

namespace {

// Cookie-style wide memory stream. The File class emits encoded bytes (UTF-8,
// or single-byte codepoints in non-UTF-8 locales) through platform_write; we
// reverse the encoding into a caller-owned wchar_t buffer. *ptrp / *sizep are
// republished on every fflush and on fclose (POSIX open_wmemstream).
class WmemstreamFile : public File {
  // Byte-side I/O buffer owned by File (freed via own_buf=true). 256 bytes
  // amortises the byte-at-a-time writes the wide encoder emits without
  // pinning a full 1 KiB per active wmemstream.
  static constexpr size_t BYTE_BUFFER_SIZE = 256;

  wchar_t **ptrp;
  size_t *sizep;

  // Caller-owned wchar buffer, grown via realloc, freed by the caller after
  // fclose. wcap counts wchar_t slots including the trailing NUL slot;
  // wlen is the current write position; wend is the high-water mark reported
  // as *sizep.
  wchar_t *wbuf;
  size_t wcap;
  size_t wlen;
  size_t wend;

  // Persistent decode state for the byte → wchar reverse conversion. File's
  // write buffer can split a multi-byte sequence across platform_write calls
  // so we must carry partial state between calls.
  internal::mbstate decode_state;
  bool decode_fatal; // Sticky EILSEQ.

  static FileIOResult wm_write(File *f, const void *data, size_t size);
  static ErrorOr<off_t> wm_seek(File *f, off_t offset, int whence);
  static int wm_sync(File *f);
  static int wm_close(File *f);

  bool ensure_capacity(size_t needed);
  bool publish();

public:
  static constexpr size_t BYTE_BUFFER_BYTES = BYTE_BUFFER_SIZE;

  WmemstreamFile(wchar_t **p, size_t *s, uint8_t *byte_buffer)
      : File(&wm_write, /*rf=*/nullptr, &wm_seek, &wm_close, byte_buffer,
             BYTE_BUFFER_SIZE, _IOFBF, /*owned=*/true,
             static_cast<ModeFlags>(OpenMode::WRITE), &wm_sync),
        ptrp(p), sizep(s), wbuf(nullptr), wcap(0), wlen(0), wend(0),
        decode_state{}, decode_fatal(false) {}

  // Allocate the minimum 1-wchar NUL buffer and publish *ptrp/*sizep so the
  // caller sees valid storage the moment open_wmemstream returns.
  bool initial_publish() { return publish(); }
};

bool WmemstreamFile::ensure_capacity(size_t needed) {
  if (needed <= wcap)
    return true;
  size_t new_cap = wcap ? wcap : 64;
  while (new_cap < needed)
    new_cap *= 2;
  // realloc(nullptr, n) behaves as malloc(n).
  void *p = ::realloc(wbuf, new_cap * sizeof(wchar_t));
  if (!p)
    return false;
  wbuf = static_cast<wchar_t *>(p);
  wcap = new_cap;
  return true;
}

bool WmemstreamFile::publish() {
  if (!ensure_capacity(wend + 1))
    return false;
  wbuf[wend] = L'\0';
  *ptrp = wbuf;
  *sizep = wend;
  return true;
}

FileIOResult WmemstreamFile::wm_write(File *f, const void *data, size_t size) {
  auto *self = static_cast<WmemstreamFile *>(f);
  if (self->decode_fatal)
    return {0, EILSEQ};
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  internal::CharacterConverter cr(&self->decode_state,
                                  internal::locale_encoding_is_utf8());
  for (size_t i = 0; i < size; ++i) {
    // Reserve room for the wchar this byte might complete (plus the NUL slot)
    // BEFORE feeding it to the decoder. This keeps push+pop+store atomic:
    // on ENOMEM we return with no byte consumed and the partial mbstate
    // intact, so a later retry — or a fresh call — resumes cleanly.
    if (!self->ensure_capacity(self->wlen + 2))
      return {i, ENOMEM};
    int push_err = cr.push(static_cast<char8_t>(bytes[i]));
    if (push_err != 0) {
      cr.clear();
      self->decode_fatal = true;
      return {i, EILSEQ};
    }
    if (!cr.isFull())
      continue;
    auto cp = cr.pop_utf32();
    if (!cp.has_value()) {
      cr.clear();
      self->decode_fatal = true;
      return {i, EILSEQ};
    }
    self->wbuf[self->wlen++] = static_cast<wchar_t>(cp.value());
    if (self->wlen > self->wend)
      self->wend = self->wlen;
  }
  return {size, 0};
}

ErrorOr<off_t> WmemstreamFile::wm_seek(File *f, off_t offset, int whence) {
  auto *self = static_cast<WmemstreamFile *>(f);
  // POSIX treats the wmemstream's file position indicator as a wide-char
  // count — fseek/ftell values round-trip through wlen, not byte offsets.
  off_t base;
  switch (whence) {
  case SEEK_SET:
    base = 0;
    break;
  case SEEK_CUR:
    base = static_cast<off_t>(self->wlen);
    break;
  case SEEK_END:
    base = static_cast<off_t>(self->wend);
    break;
  default:
    return Error(EINVAL);
  }
  // Guard against signed overflow in base+offset (UB on off_t) and against
  // a target wchar count whose byte size would wrap size_t on the way to
  // realloc. Both are reachable via a single fseeko(stream, INT64_MAX, ...).
  off_t new_pos;
  if (__builtin_add_overflow(base, offset, &new_pos))
    return Error(EOVERFLOW);
  if (new_pos < 0)
    return Error(EINVAL);
  size_t target = static_cast<size_t>(new_pos);
  if (target > (SIZE_MAX / sizeof(wchar_t)) - 1)
    return Error(ENOMEM);
  // Extending past the high-water mark NUL-fills the gap (POSIX: holes
  // read as zero wide characters once the stream is re-read).
  if (target > self->wend) {
    if (!self->ensure_capacity(target + 1))
      return Error(ENOMEM);
    for (size_t i = self->wend; i < target; ++i)
      self->wbuf[i] = L'\0';
    self->wend = target;
  }
  self->wlen = target;
  // A seek invalidates any in-flight multi-byte sequence on the byte side:
  // subsequent encoder output lands at the new wlen, not as a continuation
  // of whatever partial state was left over. Also clears a sticky EILSEQ so
  // a caller that rewinds past the bad write can reuse the stream.
  self->decode_state = internal::mbstate{};
  self->decode_fatal = false;
  return new_pos;
}

int WmemstreamFile::wm_sync(File *f) {
  auto *self = static_cast<WmemstreamFile *>(f);
  return self->publish() ? 0 : ENOMEM;
}

int WmemstreamFile::wm_close(File *f) {
  // File::close() already flushed (which ran wm_sync → publish), so the
  // caller-visible *ptrp/*sizep are already current. wbuf ownership passes
  // to the caller; only the File object itself is ours to release.
  delete static_cast<WmemstreamFile *>(f);
  return 0;
}

} // anonymous namespace

LLVM_LIBC_FUNCTION(::FILE *, open_wmemstream,
                   (wchar_t * *ptrp, size_t *sizep)) {
  if (ptrp == nullptr || sizep == nullptr)
    return nullptr;

  uint8_t *byte_buffer;
  {
    AllocChecker ac;
    byte_buffer = new (ac) uint8_t[WmemstreamFile::BYTE_BUFFER_BYTES];
    if (!ac)
      return nullptr;
  }
  AllocChecker ac;
  auto *file = new (ac) WmemstreamFile(ptrp, sizep, byte_buffer);
  if (!ac) {
    delete[] byte_buffer;
    return nullptr;
  }

  // Claim wide orientation up front so stray byte I/O (fputc, fwrite) trips
  // adopt_orientation instead of corrupting our decode state.
  file->fwide(1);

  // Publish an empty NUL-terminated buffer so *ptrp/*sizep are valid before
  // the first write (POSIX only leaves contents unspecified *after* writes).
  if (!file->initial_publish()) {
    file->close();
    return nullptr;
  }

  // Register with the global file list so fflush(NULL) and the exit-time
  // flush-all see this stream. POSIX open_wmemstream §: the buffer contents
  // are observable through *ptrp/*sizep only after a successful fflush/fclose
  // — without this, a caller that forgets fflush before exit sees stale size.
  File::add_file(file);
  return reinterpret_cast<::FILE *>(file);
}

} // namespace LIBC_NAMESPACE_DECL
