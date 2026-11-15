// Intentionally invalid bootstrap outputs; never installed as a supported Plugin.
#include <rodaq/plugin/c/plugin.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

namespace {

bool mode(const char* expected) {
    const auto* value = std::getenv("RODAQ_LOADER_FAULT");
    return value != nullptr && std::strcmp(value, expected) == 0;
}

void marker(const char* variable) {
    const auto* path = std::getenv(variable);
    if (path != nullptr) {
        const auto file = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        if (file >= 0) {
            close(file);
        }
    }
}

__attribute__((constructor)) void block_initializer() {
    if (!mode("hang_load")) {
        return;
    }
    marker("RODAQ_LOADER_ENTERED");
    const auto* gate = std::getenv("RODAQ_LOADER_GATE");
    const auto file = gate == nullptr ? -1 : open(gate, O_RDONLY | O_CLOEXEC);
    if (file < 0) {
        std::_Exit(63);
    }
    char byte = 0;
    const auto count = read(file, &byte, 1);
    close(file);
    if (count != 1) {
        std::_Exit(64);
    }
}

__attribute__((destructor)) void record_rollback() {
    if (mode("hang_load")) {
        marker("RODAQ_LOADER_UNLOADED");
    }
}

struct GuardedHeader {
    GuardedHeader() {
        size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        mapping =
            mmap(nullptr, size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping == MAP_FAILED ||
            mprotect(static_cast<char*>(mapping) + size, size, PROT_NONE)) {
            std::_Exit(60);
        }
        header = new (static_cast<char*>(mapping) + size - 8) rodaq_struct_header_t{4};
    }

    ~GuardedHeader() { munmap(mapping, size * 2); }

    void* mapping{nullptr};
    std::size_t size{0};
    rodaq_struct_header_t* header{nullptr};
};

const rodaq_plugin_descriptor_t* descriptor() {
    if (mode("short_descriptor")) {
        static GuardedHeader guarded;
        return reinterpret_cast<const rodaq_plugin_descriptor_t*>(guarded.header);
    }
    static const rodaq_plugin_descriptor_t valid{
        {sizeof(rodaq_plugin_descriptor_t)},
        {"com.example.counter", 19},
        {"C++ Counter", 11},
        {"1.0.0", 5}};
    static const rodaq_plugin_descriptor_t mismatch{
        {sizeof(rodaq_plugin_descriptor_t)},
        {"com.example.different", 21},
        {"C++ Counter", 11},
        {"1.0.0", 5}};
    return mode("descriptor_mismatch") ? &mismatch : &valid;
}

rodaq_status_code_t create(rodaq_plugin_instance_t*, rodaq_error_t*) {
    std::_Exit(61);  // Loader must never invoke create while validating bootstrap.
}

}  // namespace

extern "C" RODAQ_PLUGIN_EXPORT_SYMBOL int rodaq_loader_dependency_anchor() { return 1; }

extern "C" uint32_t rodaq_plugin_abi_version() {
    if (mode("throw_version")) {
        throw std::bad_alloc{};
    }
    if (mode("major")) {
        return RODAQ_PLUGIN_ABI_ENCODE(2, 0);
    }
    return mode("minor") ? RODAQ_PLUGIN_ABI_ENCODE(1, 1) : RODAQ_PLUGIN_ABI_VERSION;
}

extern "C" const rodaq_plugin_entry_t* rodaq_plugin_get_entry() {
    if (mode("major") || mode("minor")) {
        std::_Exit(62);  // Version must be checked before calling this symbol.
    }
    if (mode("throw_entry")) {
        throw std::bad_alloc{};
    }
    if (mode("null_entry")) {
        return nullptr;
    }
    if (mode("short_entry")) {
        static GuardedHeader guarded;
        return reinterpret_cast<const rodaq_plugin_entry_t*>(guarded.header);
    }
    static const rodaq_plugin_entry_t valid{{sizeof(rodaq_plugin_entry_t)}, &descriptor, &create};
    static const rodaq_plugin_entry_t missing{{sizeof(rodaq_plugin_entry_t)}, &descriptor, nullptr};
    if (mode("misaligned_entry")) {
        return reinterpret_cast<const rodaq_plugin_entry_t*>(
            reinterpret_cast<const char*>(&valid) + 1);
    }
    return mode("missing_slot") ? &missing : &valid;
}
