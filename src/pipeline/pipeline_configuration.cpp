#include "pipeline_controller_state.hpp"

#include <mutex>
#include <new>
#include <utility>

#include "../persistence/json.hpp"

namespace rodaq::pipeline {

Result<void> PipelineController::State::freeze_effective_configuration() {
    if (!options.manifest.effective_configuration) {
        return {};
    }
    try {
        auto effective = *options.manifest.effective_configuration;
        auto observations = persistence::Json::array();
        for (const auto& binding : output->instance->streams.bindings()) {
            auto* stream = output->instance->streams.find(binding.stream_id);
            DescriptorRef descriptor;
            {
                std::lock_guard<std::mutex> lock(stream->mutex);
                descriptor = stream->active_descriptor;
            }
            if (!descriptor) {
                return Status(StatusCode::Unavailable, {});
            }
            const auto& facts = descriptor->facts;
            persistence::Json observed{
                {"stream_id", std::to_string(binding.stream_id)},
                {"stream_key", std::string(binding.stream_key.data, binding.stream_key.size)},
                {"data_profile", std::to_string(facts.data_profile)},
                {"channel_name", facts.channel_name},
                {"schema_name", facts.schema_name},
                {"schema_encoding", facts.schema_encoding},
                {"schema_digest", facts.schema_digest},
                {"message_encoding", facts.message_encoding}};
            if (facts.ros2_subscription) {
                auto subscription = canonical::subscription_document(*facts.ros2_subscription);
                // Generation associates runtime observations; it is not an
                // effective setting and must not make equivalent settings differ.
                subscription.erase("subscription_generation");
                observed["ros2_subscription"] = std::move(subscription);
            }
            observations.push_back(std::move(observed));
        }
        effective["prepared_streams"] = std::move(observations);
        auto identity = persistence::semantic_digest(effective, "rodaq.effective-config.v1");
        if (!identity.ok()) {
            return identity.status();
        }
        auto segment = std::atomic_load(&output->segment);
        if (!segment) {
            return Status(StatusCode::InvalidState, {});
        }
        auto bound = segment->metadata.bind_effective_configuration(identity.value(), effective);
        if (!bound.ok()) {
            return bound.status();
        }
        // Gate/Open only reads the key under open_mutex. Preparation owns the
        // remaining immutable instance configuration until it actually returns.
        options.manifest.effective_configuration = std::move(effective);
        {
            std::lock_guard<std::mutex> lock(open_mutex);
            options.manifest.effective_config_id = std::move(identity.value());
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status(StatusCode::ResourceExhausted, {});
    }
}

}  // namespace rodaq::pipeline
