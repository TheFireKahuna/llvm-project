//===-- winmain_main.cpp - WinMain of a main() GUI program ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The WinMain of a GUI program that defines none, which calls its main. A file
// of its own, so that a program with a WinMain does not reference main.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

int main(int, char **, char **);

extern "C" int WINAPI __wincrt_WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return main(*__p___argc(), *__p___argv(), *__p__environ());
}
