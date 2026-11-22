#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <sys/types.h>

#include <mcap/writer.hpp>
#include <rodaq/result.hpp>

namespace rodaq::mcap_storage {

// Writer-lane owner. Never fsyncs, publishes or unlinks Core-managed files.
// Logical MCAP offsets keep advancing after an I/O error; confirmed bytes do
// not. Once failed, no later format operation can resume physical mutation.
class CheckedSink final : public mcap::IWritable {
public:
    using Write = std::function<ssize_t(int, const void*, std::size_t)>;

    explicit CheckedSink(Write write = {});
    CheckedSink(const CheckedSink&) = delete;
    CheckedSink& operator=(const CheckedSink&) = delete;
    ~CheckedSink() noexcept override;

    Result<void> open(int directory, const char* name);
    void end() override;
    std::uint64_t size() const override;
    std::uint64_t confirmed_bytes() const noexcept;
    StatusCode status() const noexcept;

private:
    void handleWrite(const std::byte* data, std::uint64_t size) override;
    void fail(StatusCode error) noexcept;

    Write write_;
    int file_{-1};
    std::uint64_t logical_{0};
    std::uint64_t confirmed_{0};
    StatusCode status_{StatusCode::Ok};
};

}  // namespace rodaq::mcap_storage
