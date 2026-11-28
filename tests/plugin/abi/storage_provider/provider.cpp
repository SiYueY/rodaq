#include <rodaq/plugin/authoring.hpp>
#include <rodaq/storage/interface.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace storage = rodaq::storage;
using rodaq::plugin::Deadline;

struct StorageProbe {
    rodaq::Result<void> prepare(const storage::PrepareConfig&, Deadline) { return {}; }

    rodaq::Result<void> prepare_descriptor(const storage::StreamDescriptor&, Deadline) {
        return {};
    }

    rodaq::Result<void> open_segment(const storage::SegmentSpec&, Deadline) { return {}; }

    storage::WriteOutcome write_batch(storage::Batch batch, Deadline) {
        if (mode == "partial_unwritten" || mode == "partial_unknown") {
            return {
                rodaq::Status(rodaq::StatusCode::Unavailable, "partial write"),
                {{sizeof(storage::BatchResult)},
                 1,
                 mode == "partial_unknown" ? RODAQ_STORAGE_TAIL_UNKNOWN
                                           : RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN}};
        }
        if (mode == "full_failed") {
            return {
                rodaq::Status(rodaq::StatusCode::Unavailable, "operation failed after full prefix"),
                {{sizeof(storage::BatchResult)}, batch.count, RODAQ_STORAGE_TAIL_NONE}};
        }
        return {
            rodaq::Status{},
            {{sizeof(storage::BatchResult)}, batch.count, RODAQ_STORAGE_TAIL_NONE}};
    }

    rodaq::Result<storage::SegmentUsage> segment_usage() const {
        return storage::SegmentUsage{{sizeof(storage::SegmentUsage)}, 1, 1, 0};
    }

    rodaq::Result<storage::SegmentSealResult> seal_segment(Deadline) {
        ++seal_calls;
        return storage::SegmentSealResult{{{"probe.bin", seal_calls}}, seal_calls};
    }

    rodaq::Result<void> quiesce(Deadline) { return {}; }

    std::string mode;
    std::uint64_t seal_calls{0};
};

template <typename Implementation>
struct RawProbeProvider : storage::Provider<Implementation> {
    static rodaq_status_code_t write_batch(
        rodaq_interface_context_t* context, const storage::Record* records, std::uint32_t count,
        rodaq_deadline_t deadline, storage::BatchResult* result, rodaq_error_t* error) {
        const auto& mode = reinterpret_cast<Implementation*>(context)->mode;
        if (mode == "throw_write") {
            throw std::runtime_error("deliberate illegal ABI exception");
        }
        const auto code = storage::Provider<Implementation>::write_batch(
            context, records, count, deadline, result, error);
        if (mode == "bad_batch") {
            result->confirmed_prefix = count + 1;
        }
        if (mode == "unknown_status") {
            return 99;
        }
        if (mode == "bad_error") {
            result->confirmed_prefix = 1;
            result->tail_state = RODAQ_STORAGE_TAIL_UNKNOWN;
            error->message.data = reinterpret_cast<std::uint8_t*>(1);
            error->message.size = error->message.capacity;
            return RODAQ_STATUS_UNAVAILABLE;
        }
        return code;
    }

    static rodaq_status_code_t seal_segment(
        rodaq_interface_context_t* context, rodaq_deadline_t deadline,
        rodaq_segment_seal_result_t* output, rodaq_error_t* error) {
        const auto code =
            storage::Provider<Implementation>::seal_segment(context, deadline, output, error);
        if (code != RODAQ_STATUS_OK) {
            return code;
        }
        const auto& mode = reinterpret_cast<Implementation*>(context)->mode;
        if (mode == "bad_files_pointer") {
            output->files = reinterpret_cast<rodaq_sealed_file_t*>(1);
        } else if (mode == "bad_used_pointer") {
            output->files[0].relative_path.data = reinterpret_cast<char*>(1);
        } else if (mode == "bad_unused_pointer") {
            output->files[1].relative_path.data = reinterpret_cast<char*>(1);
        } else if (mode == "bad_unused_capacity") {
            ++output->files[1].relative_path.capacity;
        } else if (mode == "bad_unused_bytes") {
            output->files[1].relative_path.data[1] = 'x';
        } else if (mode == "bad_seal_total") {
            ++output->total_physical_bytes;
        } else if (mode == "bad_seal_count") {
            output->file_count = output->file_capacity + 1;
        }
        return code;
    }

    static const rodaq_storage_interface_vtable_t* vtable() noexcept {
        static const auto table = [] {
            auto value = *storage::Provider<Implementation>::vtable();
            value.write_batch = write_batch;
            value.seal_segment = seal_segment;
            return value;
        }();
        return &table;
    }
};

struct ProbeInterface : storage::Interface {
    template <typename Implementation>
    using Provider = RawProbeProvider<Implementation>;
};

class ProbePlugin final : public rodaq::plugin::PluginBase {
public:
    rodaq::Result<void> configure(rodaq::plugin::PluginSetup& setup) override {
        try {
            const auto config = nlohmann::json::parse(setup.config());
            probe_.mode = config.value("mode", "normal");
        } catch (const nlohmann::json::exception&) {
            return rodaq::Status(
                rodaq::StatusCode::InvalidArgument, "Invalid ABI probe configuration");
        }
        return setup.provide<ProbeInterface>(probe_);
    }

    rodaq::Result<void> shutdown(Deadline) override { return {}; }

private:
    StorageProbe probe_;
};

RODAQ_DEFINE_PLUGIN(ProbePlugin)
