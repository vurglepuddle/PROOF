// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PROOF_GK_STATUS_H
#define PROOF_GK_STATUS_H
#include <optional>
#include <string>
#include <utility>

namespace Proof::GK {
enum class Status { Ok, OkEmpty, Cancelled, LimitExceeded, Unsupported, InvalidInput, NumericalFailure };
// Only the numeric scaffold. Diagnostics and the public kernel Outcome wait for the merge.
template <typename T> struct Result {
    Status status;
    std::optional<T> value;
    std::string detail;
    explicit operator bool() const { return status == Status::Ok && value.has_value(); }
    static Result success(T v) { return {Status::Ok, std::move(v), {}}; }
    static Result failure(Status s, std::string message) { return {s, std::nullopt, std::move(message)}; }
};
} // namespace Proof::GK
#endif
