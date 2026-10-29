#include "doctest.h"

#include "infer/hardware.hpp"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace dale::infer;
namespace fetch = dale::fetch;

namespace {

constexpr std::uint64_t kMiB = 1024ull * 1024;

Gpu gpu(const std::string& description, std::uint64_t free_mib, bool integrated = false) {
    Gpu g;
    g.name = "Vulkan-" + description;
    g.description = description;
    g.integrated = integrated;
    g.free_bytes = free_mib * kMiB;
    g.total_bytes = 2 * free_mib * kMiB;
    return g;
}

bool cpu_choice(const Choice& c) { return !c.gpu && c.model == &fetch::kDefaultModel; }

// Saves an environment variable and restores it when going out of scope.
class EnvGuard {
public:
    explicit EnvGuard(const char* name) : name_(name) {
        if (const char* v = std::getenv(name)) old_ = v;
    }
    ~EnvGuard() {
        if (old_) setenv(name_, old_->c_str(), 1);
        else unsetenv(name_);
    }
    void set(const std::string& v) { setenv(name_, v.c_str(), 1); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    const char* name_;
    std::optional<std::string> old_;
};

}  // namespace

TEST_SUITE("hardware") {

TEST_CASE("choose: sin GPU, CPU con dale-0.8b (automático y forzado)") {
    CHECK(cpu_choice(choose({}, false)));
    CHECK(cpu_choice(choose({}, true)));
}

TEST_CASE("choose: solo una iGPU se ignora salvo forzada, y nunca con dale-9b") {
    const std::vector<Gpu> igpu{gpu("Intel UHD", 16000, true)};
    CHECK(cpu_choice(choose(igpu, false)));

    const Choice forced = choose(igpu, true);
    CHECK(forced.gpu);
    CHECK(forced.gpu_index == 0);
    CHECK(forced.model == &fetch::kDale4B_1_0_0_Q4_K_M);

    // Forced iGPU with little memory: dale-0.8b on the GPU.
    const Choice small = choose({gpu("Intel UHD", 2000, true)}, true);
    CHECK(small.gpu);
    CHECK(small.model == &fetch::kDefaultModel);
}

TEST_CASE("choose: modelo según la VRAM libre de una GPU dedicada") {
    const Choice three = choose({gpu("RTX", 3 * 1024)}, false);
    CHECK(cpu_choice(three));

    const Choice six = choose({gpu("RTX", 6 * 1024)}, false);
    CHECK(six.gpu);
    CHECK(six.gpu_index == 0);
    CHECK(six.model == &fetch::kDale4B_1_0_0_Q4_K_M);

    const Choice ten = choose({gpu("RTX", 10 * 1024)}, false);
    CHECK(ten.gpu);
    CHECK(ten.model == &fetch::kDale9B_1_0_0_Q4_K_M);
}

TEST_CASE("choose: umbrales exactos de 4 y 8 GiB") {
    Gpu g = gpu("RTX", 0);
    g.free_bytes = kMinFree4B - 1;
    CHECK(cpu_choice(choose({g}, false)));
    g.free_bytes = kMinFree4B;
    CHECK(choose({g}, false).model == &fetch::kDale4B_1_0_0_Q4_K_M);
    g.free_bytes = kMinFree9B - 1;
    CHECK(choose({g}, false).model == &fetch::kDale4B_1_0_0_Q4_K_M);
    g.free_bytes = kMinFree9B;
    CHECK(choose({g}, false).model == &fetch::kDale9B_1_0_0_Q4_K_M);
}

TEST_CASE("choose: con varias GPU, la dedicada con más VRAM libre") {
    const Choice two = choose({gpu("RTX A", 5000), gpu("RTX B", 10000)}, false);
    CHECK(two.gpu);
    CHECK(two.gpu_index == 1);
    CHECK(two.model == &fetch::kDale9B_1_0_0_Q4_K_M);

    const Choice first = choose({gpu("RTX A", 10000), gpu("RTX B", 5000)}, false);
    CHECK(first.gpu_index == 0);
    CHECK(first.model == &fetch::kDale9B_1_0_0_Q4_K_M);

    // A large iGPU does not beat a dedicated one, neither in auto nor in forced mode.
    const std::vector<Gpu> mixed{gpu("Intel UHD", 20000, true), gpu("RTX", 5000)};
    for (bool forced : {false, true}) {
        CAPTURE(forced);
        const Choice c = choose(mixed, forced);
        CHECK(c.gpu);
        CHECK(c.gpu_index == 1);
        CHECK(c.model == &fetch::kDale4B_1_0_0_Q4_K_M);
    }

    // Forced: the dedicated one wins even with less VRAM than the iGPU.
    const Choice low = choose({gpu("RTX", 2000), gpu("Intel UHD", 20000, true)}, true);
    CHECK(low.gpu);
    CHECK(low.gpu_index == 0);
    CHECK(low.model == &fetch::kDefaultModel);
    CHECK(cpu_choice(choose({gpu("RTX", 2000), gpu("Intel UHD", 20000, true)}, false)));
}

TEST_CASE("choose: forzada con poca VRAM usa dale-0.8b en la GPU") {
    const Choice c = choose({gpu("GTX", 1500)}, true);
    CHECK(c.gpu);
    CHECK(c.gpu_index == 0);
    CHECK(c.model == &fetch::kDefaultModel);
    // Forced with plenty of VRAM: the same models as in auto mode.
    CHECK(choose({gpu("RTX", 6000)}, true).model == &fetch::kDale4B_1_0_0_Q4_K_M);
    CHECK(choose({gpu("RTX", 12000)}, true).model == &fetch::kDale9B_1_0_0_Q4_K_M);
}

#if DALE_TEST_HOOKS
TEST_CASE("detect_gpus: DALE_FAKE_GPU") {
    EnvGuard fake("DALE_FAKE_GPU");

    fake.set("NVIDIA GeForce RTX 5070 Ti:12000:16000;Intel UHD 770:2048:8192:igpu");
    const std::vector<Gpu> two = detect_gpus();
    REQUIRE(two.size() == 2);
    CHECK(two[0].name == "Fake0");
    CHECK(two[0].description == "NVIDIA GeForce RTX 5070 Ti");
    CHECK(two[0].free_bytes == 12000 * kMiB);
    CHECK(two[0].total_bytes == 16000 * kMiB);
    CHECK_FALSE(two[0].integrated);
    CHECK(two[1].name == "Fake1");
    CHECK(two[1].description == "Intel UHD 770");
    CHECK(two[1].free_bytes == 2048 * kMiB);
    CHECK(two[1].total_bytes == 8192 * kMiB);
    CHECK(two[1].integrated);

    fake.set("");
    CHECK(detect_gpus().empty());

    // Malformed entries (fewer than three fields) are skipped; other suffixes are not "igpu".
    fake.set("solo nombre;A:1;;B:100:200:dgpu;C:300:400");
    const std::vector<Gpu> some = detect_gpus();
    REQUIRE(some.size() == 2);
    CHECK(some[0].description == "B");
    CHECK_FALSE(some[0].integrated);
    CHECK(some[1].description == "C");
    CHECK(some[1].name == "Fake1");
    CHECK(some[1].free_bytes == 300 * kMiB);

    // Through choose(): the fake 5070 Ti gets dale-9b.
    fake.set("Intel UHD:30000:32000:igpu;RTX:12000:16000");
    const std::vector<Gpu> gpus = detect_gpus();
    const Choice c = choose(gpus, false);
    CHECK(c.gpu);
    CHECK(gpus[c.gpu_index].description == "RTX");
    CHECK(c.model == &fetch::kDale9B_1_0_0_Q4_K_M);
}
#endif

}  // TEST_SUITE("hardware")
