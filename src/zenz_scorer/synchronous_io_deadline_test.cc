#include "zenz_scorer/synchronous_io_deadline.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "testing/gunit.h"

namespace mozc::zenz {
namespace {

void CheckStalledPeer(bool flush) {
  static std::atomic<int> sequence{0};
  const std::wstring name = L"\\\\.\\pipe\\mozc_deadline_test_" +
                            std::to_wstring(::GetCurrentProcessId()) + L"_" +
                            std::to_wstring(sequence.fetch_add(1));
  HANDLE pipe =
      ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                         PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
  ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
  std::atomic<bool> done{false};
  std::thread peer([&] {
    HANDLE client = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    if (client != INVALID_HANDLE_VALUE) {
      // Finite fallback makes a broken watchdog fail instead of hanging tests.
      for (int i = 0; i < 3000 && !done.load(); ++i) {
        ::Sleep(1);
      }
      ::CloseHandle(client);
    }
  });
  const bool connected = ::ConnectNamedPipe(pipe, nullptr) ||
                         ::GetLastError() == ERROR_PIPE_CONNECTED;
  EXPECT_TRUE(connected);
  if (connected) {
    char byte = 'x';
    DWORD transferred = 0;
    if (flush) {
      EXPECT_TRUE(::WriteFile(pipe, &byte, 1, &transferred, nullptr));
    }
    const auto started = std::chrono::steady_clock::now();
    {
      SynchronousIoDeadline deadline(pipe, std::chrono::milliseconds(50));
      EXPECT_TRUE(deadline.valid());
      const BOOL ok = flush ? ::FlushFileBuffers(pipe)
                            : ::ReadFile(pipe, &byte, 1, &transferred, nullptr);
      const DWORD error = ::GetLastError();
      EXPECT_FALSE(ok);
      EXPECT_EQ(error, ERROR_OPERATION_ABORTED);
      if (!flush) {
        // Cancellation must also cover an I/O issued after the first timeout.
        EXPECT_FALSE(::ReadFile(pipe, &byte, 1, &transferred, nullptr));
        EXPECT_EQ(::GetLastError(), ERROR_OPERATION_ABORTED);
      }
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              std::chrono::seconds(2));
  }
  done = true;
  peer.join();
  ::DisconnectNamedPipe(pipe);
  ::CloseHandle(pipe);
}

TEST(SynchronousIoDeadlineTest, CancelsStalledRead) { CheckStalledPeer(false); }

TEST(SynchronousIoDeadlineTest, CancelsUnreadResponseFlush) {
  CheckStalledPeer(true);
}

TEST(SynchronousIoDeadlineTest, CompletedOperationDoesNotWaitForDeadline) {
  const auto started = std::chrono::steady_clock::now();
  {
    SynchronousIoDeadline deadline(std::chrono::seconds(30));
    EXPECT_TRUE(deadline.valid());
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(2));
}

}  // namespace
}  // namespace mozc::zenz
