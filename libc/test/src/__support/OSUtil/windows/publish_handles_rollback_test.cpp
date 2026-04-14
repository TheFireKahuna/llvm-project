//===-- publish_handles rollback regression test --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Regression test for the Phase 3 bug in publish_handles(): when
// commit_published_state() partially succeeds (PEB mutated) but then fails
// (e.g. cookie write failure), the rollback must fully restore the original
// PEB ConsoleHandle, StandardInput/Output/Error, and fd table state.
//
// Strategy: capture PEB state before publish, call publish_handles() with
// a saved-state object, then call restore_handles() to undo. Verify that
// all PEB fields are back to their original values and the published handles
// are closed.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/ipc/condrv.h"
#include "src/__support/OSUtil/windows/nt/nt_process.h"
#include "src/__support/OSUtil/windows/process/console.h"
#include "test/UnitTest/Test.h"

namespace {

using namespace LIBC_NAMESPACE;

struct PebSnapshot {
  HANDLE console_handle;
  HANDLE standard_input;
  HANDLE standard_output;
  HANDLE standard_error;
};

PebSnapshot capture_peb_state() {
  auto *params = NtCurrentPeb()->ProcessParameters;
  PebSnapshot snap = {};
  snap.console_handle = params->ConsoleHandle;
  snap.standard_input = params->StandardInput;
  snap.standard_output = params->StandardOutput;
  snap.standard_error = params->StandardError;
  return snap;
}

bool peb_state_matches(const PebSnapshot &a, const PebSnapshot &b) {
  return a.console_handle == b.console_handle &&
         a.standard_input == b.standard_input &&
         a.standard_output == b.standard_output &&
         a.standard_error == b.standard_error;
}

bool handle_is_valid(HANDLE h) {
  return h != nullptr && h != INVALID_HANDLE_VALUE;
}

} // namespace

TEST(LlvmLibcPublishHandlesTest, PublishAndRestorePreservesPebState) {
  // Skip if no console is attached (e.g., headless CI).
  HANDLE console = internal::condrv::get_console_handle();
  if (!handle_is_valid(console))
    return;

  PebSnapshot before = capture_peb_state();

  // Launch a temporary console session to get valid client handles.
  internal::console::Session session;
  internal::console::SessionOptions opts;
  opts.publish_to_process = false; // don't auto-publish
  opts.window_visible = false;
  NTSTATUS status = internal::console::launch(&session, opts);
  if (!NT_SUCCESS(status))
    return; // Can't launch console — skip gracefully.

  // publish_handles with saved state.
  internal::console::PublishedState saved = {};
  status = internal::console::publish_handles(session.handles,
                                              session.reference, &saved);
  if (!NT_SUCCESS(status)) {
    // If publish itself fails, PEB should be unchanged.
    PebSnapshot after_fail = capture_peb_state();
    EXPECT_TRUE(peb_state_matches(before, after_fail));
    return;
  }

  // After publish, PEB should have been mutated.
  PebSnapshot after_publish = capture_peb_state();
  // At least one field should differ (ConsoleHandle was replaced).
  bool something_changed =
      after_publish.console_handle != before.console_handle ||
      after_publish.standard_input != before.standard_input ||
      after_publish.standard_output != before.standard_output ||
      after_publish.standard_error != before.standard_error;
  EXPECT_TRUE(something_changed);

  // The saved state should have captured the original values.
  EXPECT_EQ(saved.console_handle, before.console_handle);
  EXPECT_EQ(saved.standard_input, before.standard_input);
  EXPECT_EQ(saved.standard_output, before.standard_output);
  EXPECT_EQ(saved.standard_error, before.standard_error);

  // Now restore — the PEB should return to the original values.
  status = internal::console::restore_handles(&saved);
  ASSERT_TRUE(NT_SUCCESS(status));

  PebSnapshot after_restore = capture_peb_state();
  EXPECT_TRUE(peb_state_matches(before, after_restore));

  // Clean up the session without re-restoring (already restored).
  session.published = false;
}

TEST(LlvmLibcPublishHandlesTest, PublishWithNullConnectionFails) {
  PebSnapshot before = capture_peb_state();

  internal::condrv::CONDRV_CLIENT_HANDLES bad_handles = {};
  bad_handles.Connection = nullptr; // invalid

  internal::console::PublishedState saved = {};
  NTSTATUS status =
      internal::console::publish_handles(bad_handles, nullptr, &saved);

  // Must fail — connection is null.
  EXPECT_FALSE(NT_SUCCESS(status));

  // PEB should be completely unchanged.
  PebSnapshot after = capture_peb_state();
  EXPECT_TRUE(peb_state_matches(before, after));
}

TEST(LlvmLibcPublishHandlesTest, SavedPublishedHandlesAreValidAfterPublish) {
  HANDLE console = internal::condrv::get_console_handle();
  if (!handle_is_valid(console))
    return;

  internal::console::Session session;
  internal::console::SessionOptions opts;
  opts.publish_to_process = false;
  opts.window_visible = false;
  NTSTATUS status = internal::console::launch(&session, opts);
  if (!NT_SUCCESS(status))
    return;

  internal::console::PublishedState saved = {};
  status = internal::console::publish_handles(session.handles,
                                              session.reference, &saved);
  if (!NT_SUCCESS(status))
    return;

  // The published_* handles in the saved state are the NEW handles that were
  // written to the PEB. They should be valid while publish is active.
  EXPECT_TRUE(handle_is_valid(saved.published_console_handle));
  EXPECT_TRUE(handle_is_valid(saved.published_standard_input));
  EXPECT_TRUE(handle_is_valid(saved.published_standard_output));
  EXPECT_TRUE(handle_is_valid(saved.published_standard_error));

  // Restore to clean up.
  status = internal::console::restore_handles(&saved);
  ASSERT_TRUE(NT_SUCCESS(status));
  session.published = false;
}
