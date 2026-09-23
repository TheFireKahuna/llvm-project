//===-- image_local.h - Per-image startup contribution contract -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// These startup objects already belong to each image in an ordinary link.
// A partitioned final link instantiates their existing code and state in each
// consuming PE. The directive is link-only; it creates no runtime object or
// registration. Process-wide registries must not carry this directive.
#pragma comment(linker, "/lldimagelocal")
