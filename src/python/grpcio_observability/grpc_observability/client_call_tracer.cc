// Copyright 2023 gRPC authors.
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

#include "client_call_tracer.h"

#include <stddef.h>

#include <algorithm>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "constants.h"
#include "observability_util.h"
#include "python_observability_context.h"

#include <grpc/slice.h>

#include "src/core/lib/slice/slice.h"

namespace grpc_observability {

constexpr uint32_t PythonOpenCensusCallTracer::
    PythonOpenCensusCallAttemptTracer::kMaxTraceContextLen;
constexpr uint32_t
    PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::kMaxTagsLen;

//
// OpenCensusCallTracer
//

PythonOpenCensusCallTracer::PythonOpenCensusCallTracer(
    const char* method, const char* target, const char* trace_id,
    const char* parent_span_id, bool tracing_enabled, bool registered_method)
    : method_(GetMethod(method)),
      target_(GetTarget(target)),
      tracing_enabled_(tracing_enabled),
      registered_method_(registered_method) {
  GenerateClientContext(absl::StrCat("Sent.", method_),
                        absl::string_view(trace_id),
                        absl::string_view(parent_span_id), &context_);
}

void PythonOpenCensusCallTracer::GenerateContext() {}

void PythonOpenCensusCallTracer::RecordAnnotation(
    absl::string_view annotation) {
  if (!context_.GetSpanContext().IsSampled()) {
    return;
  }
  context_.AddSpanAnnotation(annotation);
}

void PythonOpenCensusCallTracer::RecordAnnotation(
    const Annotation& annotation) {
  if (!context_.GetSpanContext().IsSampled()) {
    return;
  }

  switch (annotation.type()) {
    // Annotations are expensive to create. We should only create it if the call
    // is being sampled by default.
    default:
      if (IsSampled()) {
        context_.AddSpanAnnotation(annotation.ToString());
      }
      break;
  }
}

PythonOpenCensusCallTracer::~PythonOpenCensusCallTracer() {
  if (PythonCensusStatsEnabled()) {
    context_.Labels().emplace_back(kClientMethod, std::string(method_));
    RecordIntMetric(kRpcClientRetriesPerCallMeasureName, retries_ - 1,
                    registered_method_,
                    context_.Labels());  // exclude first attempt
    RecordIntMetric(kRpcClientTransparentRetriesPerCallMeasureName,
                    transparent_retries_, registered_method_,
                    context_.Labels());
    RecordDoubleMetric(kRpcClientRetryDelayPerCallMeasureName,
                       ToDoubleSeconds(retry_delay_), registered_method_,
                       context_.Labels());
  }

  if (tracing_enabled_) {
    context_.EndSpan();
    if (IsSampled()) {
      RecordSpan(context_.GetSpan().ToCensusData());
    }
  }
}

PythonCensusContext
PythonOpenCensusCallTracer::CreateCensusContextForCallAttempt() {
  auto context = PythonCensusContext(absl::StrCat("Attempt.", method_),
                                     &(context_.GetSpan()), context_.Labels());
  return context;
}

PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer*
PythonOpenCensusCallTracer::StartNewAttempt(bool is_transparent_retry) {
  uint64_t attempt_num;
  {
    grpc_core::MutexLock lock(&mu_);
    if (transparent_retries_ != 0 || retries_ != 0) {
      if (PythonCensusStatsEnabled() && num_active_rpcs_ == 0) {
        retry_delay_ += absl::Now() - time_at_last_attempt_end_;
      }
    }
    attempt_num = retries_;
    if (is_transparent_retry) {
      ++transparent_retries_;
    } else {
      ++retries_;
    }
    ++num_active_rpcs_;
  }
  context_.IncreaseChildSpanCount();
  return new PythonOpenCensusCallAttemptTracer(this, attempt_num,
                                               is_transparent_retry);
}

//
// PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer
//

PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    PythonOpenCensusCallAttemptTracer(PythonOpenCensusCallTracer* parent,
                                      uint64_t attempt_num,
                                      bool is_transparent_retry)
    : parent_(parent),
      context_(parent_->CreateCensusContextForCallAttempt()),
      start_time_(absl::Now()) {
  if (parent_->tracing_enabled_) {
    context_.AddSpanAttribute("previous-rpc-attempts",
                              absl::StrCat(attempt_num));
    context_.AddSpanAttribute("transparent-retry",
                              absl::StrCat(is_transparent_retry));
  }
  if (!PythonCensusStatsEnabled()) {
    return;
  }
  context_.Labels().emplace_back(kClientMethod, std::string(parent_->method_));
  context_.Labels().emplace_back(kClientTarget, std::string(parent_->target_));
  RecordIntMetric(kRpcClientStartedRpcsMeasureName, 1,
                  parent_->registered_method_, context_.Labels());
}

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    RecordSendInitialMetadata(grpc_metadata_batch* send_initial_metadata) {
  if (parent_->tracing_enabled_) {
    char tracing_buf[kMaxTraceContextLen];
    size_t tracing_len =
        TraceContextSerialize(context_, tracing_buf, kMaxTraceContextLen);
    if (tracing_len > 0) {
      send_initial_metadata->Set(
          grpc_core::GrpcTraceBinMetadata(),
          grpc_core::Slice::FromCopiedBuffer(tracing_buf, tracing_len));
    }
  }
  if (!PythonCensusStatsEnabled()) {
    return;
  }
  grpc_slice tags = grpc_empty_slice();
  size_t encoded_tags_len = StatsContextSerialize(kMaxTagsLen, &tags);
  if (encoded_tags_len > 0) {
    send_initial_metadata->Set(grpc_core::GrpcTagsBinMetadata(),
                               grpc_core::Slice(tags));
  }
}

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    RecordSendMessage(const grpc_core::SliceBuffer& /*send_message*/) {
  ++sent_message_count_;
}

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    RecordReceivedMessage(const grpc_core::SliceBuffer& /*recv_message*/) {
  ++recv_message_count_;
}

std::shared_ptr<grpc_core::TcpTracerInterface> PythonOpenCensusCallTracer::
    PythonOpenCensusCallAttemptTracer::StartNewTcpTrace() {
  return nullptr;
}

namespace {

// Returns 0 if no server stats are present in the metadata.
uint64_t GetElapsedTimeFromTrailingMetadata(const grpc_metadata_batch* b) {
  if (!PythonCensusStatsEnabled()) {
    return 0;
  }

  const grpc_core::Slice* grpc_server_stats_bin_ptr =
      b->get_pointer(grpc_core::GrpcServerStatsBinMetadata());
  if (grpc_server_stats_bin_ptr == nullptr) {
    return 0;
  }

  uint64_t elapsed_time = 0;
  ServerStatsDeserialize(
      reinterpret_cast<const char*>(grpc_server_stats_bin_ptr->data()),
      grpc_server_stats_bin_ptr->size(), &elapsed_time);
  return elapsed_time;
}

}  // namespace

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    RecordReceivedTrailingMetadata(
        absl::Status status, grpc_metadata_batch* recv_trailing_metadata,
        const grpc_transport_stream_stats* transport_stream_stats) {
  if (!PythonCensusStatsEnabled()) {
    return;
  }
  auto status_code_ = status.code();
  uint64_t elapsed_time = 0;
  if (recv_trailing_metadata != nullptr) {
    elapsed_time = GetElapsedTimeFromTrailingMetadata(recv_trailing_metadata);
  }

  std::string final_status = absl::StatusCodeToString(status_code_);
  context_.Labels().emplace_back(kClientMethod, std::string(parent_->method_));
  context_.Labels().emplace_back(kClientTarget, std::string(parent_->target_));
  context_.Labels().emplace_back(kClientStatus, final_status);
  RecordDoubleMetric(
      kRpcClientSentBytesPerRpcMeasureName,
      static_cast<double>(transport_stream_stats != nullptr
                              ? transport_stream_stats->outgoing.data_bytes
                              : 0),
      parent_->registered_method_, context_.Labels());
  RecordDoubleMetric(
      kRpcClientReceivedBytesPerRpcMeasureName,
      static_cast<double>(transport_stream_stats != nullptr
                              ? transport_stream_stats->incoming.data_bytes
                              : 0),
      parent_->registered_method_, context_.Labels());
  RecordDoubleMetric(kRpcClientServerLatencyMeasureName,
                     absl::ToDoubleSeconds(absl::Nanoseconds(elapsed_time)),
                     parent_->registered_method_, context_.Labels());
  RecordDoubleMetric(kRpcClientRoundtripLatencyMeasureName,
                     absl::ToDoubleSeconds(absl::Now() - start_time_),
                     parent_->registered_method_, context_.Labels());
  RecordIntMetric(kRpcClientCompletedRpcMeasureName, 1,
                  parent_->registered_method_, context_.Labels());
}

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    RecordCancel(absl::Status /*cancel_error*/) {}

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::RecordEnd(
    const gpr_timespec& /*latency*/) {
  if (PythonCensusStatsEnabled()) {
    context_.Labels().emplace_back(kClientMethod,
                                   std::string(parent_->method_));
    context_.Labels().emplace_back(kClientStatus,
                                   StatusCodeToString(status_code_));
    RecordIntMetric(kRpcClientSentMessagesPerRpcMeasureName,
                    sent_message_count_, parent_->registered_method_,
                    context_.Labels());
    RecordIntMetric(kRpcClientReceivedMessagesPerRpcMeasureName,
                    recv_message_count_, parent_->registered_method_,
                    context_.Labels());

    grpc_core::MutexLock lock(&parent_->mu_);
    if (--parent_->num_active_rpcs_ == 0) {
      parent_->time_at_last_attempt_end_ = absl::Now();
    }
  }

  if (parent_->tracing_enabled_) {
    if (status_code_ != absl::StatusCode::kOk) {
      context_.GetSpan().SetStatus(StatusCodeToString(status_code_));
    }
    context_.EndSpan();
    if (IsSampled()) {
      RecordSpan(context_.GetSpan().ToCensusData());
    }
  }

  // After RecordEnd, Core will make no further usage of this CallAttemptTracer,
  // so we are free it here.
  delete this;
}

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    RecordAnnotation(absl::string_view annotation) {
  if (!context_.GetSpanContext().IsSampled()) {
    return;
  }
  context_.AddSpanAnnotation(annotation);
}

void PythonOpenCensusCallTracer::PythonOpenCensusCallAttemptTracer::
    RecordAnnotation(const Annotation& annotation) {
  if (!context_.GetSpanContext().IsSampled()) {
    return;
  }

  switch (annotation.type()) {
    // Annotations are expensive to create. We should only create it if the call
    // is being sampled by default.
    default:
      if (IsSampled()) {
        context_.AddSpanAnnotation(annotation.ToString());
      }
      break;
  }
}

}  // namespace grpc_observability
