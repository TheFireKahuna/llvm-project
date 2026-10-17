//===- ScriptLexer.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COMMON_SCRIPT_LEXER_H
#define LLD_COMMON_SCRIPT_LEXER_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBufferRef.h"

namespace lld {

// A lexer for linker scripts and version scripts, shared by the drivers. It
// turns a buffer into tokens; the driver supplies where diagnostics go, so
// each keeps its own error handling.
class ScriptLexer {
protected:
  struct Buffer {
    // The remaining content to parse and the filename.
    StringRef s, filename;
    const char *begin = nullptr;
    size_t lineNumber = 1;
    // True if the script is opened as an absolute path under the --sysroot
    // directory. The driver sets this; the lexer only carries it.
    bool isUnderSysroot = false;

    Buffer() = default;
    explicit Buffer(MemoryBufferRef mb);
  };
  // The currently lexed buffer. INCLUDE runs a nested parse on a new `Buffer`,
  // similar to a call stack frame.
  Buffer curBuf;

  // Used to detect INCLUDE() cycles.
  llvm::DenseSet<StringRef> activeFilenames;

  enum class State {
    Script,
    Expr,
    // Used by version node and dynamic list parsing.
    VersionNode,
  };

  struct Token {
    StringRef str;
    explicit operator bool() const { return !str.empty(); }
    operator StringRef() const { return str; }
  };

  // The token before the last next().
  StringRef prevTok;
  // Rules for what is a token are different when we are in an expression.
  // curTok holds the cached return value of peek() and is invalid when the
  // expression state changes.
  StringRef curTok;
  size_t prevTokLine = 1;
  // The lex state when curTok is cached.
  State curTokState = State::Script;
  State lexState = State::Script;
  bool eof = false;

  // Whether a diagnostic has already been reported; errors are treated like
  // EOF so parsing stops.
  virtual bool hasError() const = 0;
  // Reports the composed message through the driver's error handler.
  virtual void emitError(const Twine &msg) = 0;

public:
  explicit ScriptLexer(MemoryBufferRef mb);
  virtual ~ScriptLexer();

  void setError(const Twine &msg);
  void lex();
  StringRef skipSpace(StringRef s);
  bool atEOF();
  StringRef next();
  StringRef peek();
  void skip();
  bool consume(StringRef tok);
  void expect(StringRef expect);
  Token till(StringRef tok);
  std::string getCurrentLocation();

private:
  StringRef getLine();
  size_t getColumnNumber();
};

} // namespace lld

#endif
