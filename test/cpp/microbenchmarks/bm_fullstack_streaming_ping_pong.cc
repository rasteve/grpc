//
//
// Copyright 2016 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

// Benchmark gRPC end2end in various configurations

#include "test/core/test_util/build.h"
#include "test/core/test_util/test_config.h"
#include "test/cpp/microbenchmarks/fullstack_streaming_ping_pong.h"
#include "test/cpp/util/test_config.h"

namespace grpc {
namespace testing {

//******************************************************************************
// CONFIGURATIONS
//

static const int kMaxMessageSize = [] {
  if (BuiltUnderMsan() || BuiltUnderTsan() || BuiltUnderUbsan()) {
    // Scale down sizes for intensive benchmarks to avoid timeouts.
    return 8 * 1024 * 1024;
  }
  return 128 * 1024 * 1024;
}();

// Generate Args for StreamingPingPong benchmarks. Currently generates args for
// only "small streams" (i.e streams with 0, 1 or 2 messages)
static void StreamingPingPongArgs(benchmark::internal::Benchmark* b) {
  int msg_size = 0;

  b->Args({0, 0});  // spl case: 0 ping-pong msgs (msg_size doesn't matter here)

  for (msg_size = 0; msg_size <= kMaxMessageSize;
       msg_size == 0 ? msg_size++ : msg_size *= 8) {
    b->Args({msg_size, 1});
    b->Args({msg_size, 2});
  }
}

BENCHMARK_TEMPLATE(BM_StreamingPingPong, TCP, NoOpMutator, NoOpMutator)
    ->Apply(StreamingPingPongArgs);
BENCHMARK_TEMPLATE(BM_StreamingPingPong, InProcess, NoOpMutator, NoOpMutator)
    ->Apply(StreamingPingPongArgs);

BENCHMARK_TEMPLATE(BM_StreamingPingPongMsgs, TCP, NoOpMutator, NoOpMutator)
    ->Range(0, kMaxMessageSize);
BENCHMARK_TEMPLATE(BM_StreamingPingPongMsgs, InProcess, NoOpMutator,
                   NoOpMutator)
    ->Range(0, kMaxMessageSize);

BENCHMARK_TEMPLATE(BM_StreamingPingPong, MinTCP, NoOpMutator, NoOpMutator)
    ->Apply(StreamingPingPongArgs);
BENCHMARK_TEMPLATE(BM_StreamingPingPong, MinInProcess, NoOpMutator, NoOpMutator)
    ->Apply(StreamingPingPongArgs);

BENCHMARK_TEMPLATE(BM_StreamingPingPongMsgs, MinTCP, NoOpMutator, NoOpMutator)
    ->Range(0, kMaxMessageSize);
BENCHMARK_TEMPLATE(BM_StreamingPingPongMsgs, MinInProcess, NoOpMutator,
                   NoOpMutator)
    ->Range(0, kMaxMessageSize);

// Generate Args for StreamingPingPongWithCoalescingApi benchmarks. Currently
// generates args for only "small streams" (i.e streams with 0, 1 or 2 messages)
static void StreamingPingPongWithCoalescingApiArgs(
    benchmark::internal::Benchmark* b) {
  int msg_size = 0;

  b->Args(
      {0, 0, 0});  // spl case: 0 ping-pong msgs (msg_size doesn't matter here)
  b->Args(
      {0, 0, 1});  // spl case: 0 ping-pong msgs (msg_size doesn't matter here)

  for (msg_size = 0; msg_size <= kMaxMessageSize;
       msg_size == 0 ? msg_size++ : msg_size *= 8) {
    b->Args({msg_size, 1, 0});
    b->Args({msg_size, 2, 0});
    b->Args({msg_size, 1, 1});
    b->Args({msg_size, 2, 1});
  }
}

BENCHMARK_TEMPLATE(BM_StreamingPingPongWithCoalescingApi, InProcess,
                   NoOpMutator, NoOpMutator)
    ->Apply(StreamingPingPongWithCoalescingApiArgs);
BENCHMARK_TEMPLATE(BM_StreamingPingPongWithCoalescingApi, MinInProcess,
                   NoOpMutator, NoOpMutator)
    ->Apply(StreamingPingPongWithCoalescingApiArgs);

}  // namespace testing
}  // namespace grpc

// Some distros have RunSpecifiedBenchmarks under the benchmark namespace,
// and others do not. This allows us to support both modes.
namespace benchmark {
void RunTheBenchmarksNamespaced() { RunSpecifiedBenchmarks(); }
}  // namespace benchmark

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  LibraryInitializer libInit;
  ::benchmark::Initialize(&argc, argv);
  grpc::testing::InitTest(&argc, &argv, false);
  benchmark::RunTheBenchmarksNamespaced();
  return 0;
}
