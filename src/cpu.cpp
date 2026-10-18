// CPU model and instruction-set detection, shared by every OS.
// Old laptops often lack AVX/AVX2, and many prebuilt ML wheels crash with an
// "illegal instruction" on them, so the AI needs to know this up front.
#include "platform.hpp"

#include <cstring>
#include <fstream>
#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define MAAS_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace maas {

#ifdef MAAS_X86
static void run_cpuid(unsigned leaf, unsigned subleaf, unsigned registers[4]) {
#if defined(_MSC_VER)
  int values[4];
  __cpuidex(values, static_cast<int>(leaf), static_cast<int>(subleaf));
  for (int index = 0; index < 4; index++) registers[index] = static_cast<unsigned>(values[index]);
#else
  __cpuid_count(leaf, subleaf, registers[0], registers[1], registers[2], registers[3]);
#endif
}

static uint64_t read_xcr0() {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  uint32_t low = 0, high = 0;
  __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(low), "=d"(high) : "c"(0));
  return (static_cast<uint64_t>(high) << 32) | low;
#endif
}
#endif

CpuFeatures cpu_features() {
  CpuFeatures features;
#ifdef MAAS_X86
  unsigned registers[4] = {0, 0, 0, 0};
  run_cpuid(0, 0, registers);
  unsigned max_leaf = registers[0];
  if (max_leaf >= 1) {
    run_cpuid(1, 0, registers);
    unsigned ecx = registers[2];
    features.sse42 = (ecx >> 20) & 1;
    bool has_avx_bit = (ecx >> 28) & 1;
    bool has_osxsave = (ecx >> 27) & 1;
    bool has_fma = (ecx >> 12) & 1;
    // AVX is only usable if the OS saves the YMM registers (XCR0 bits 1 and 2).
    bool os_saves_ymm = false;
    bool os_saves_zmm = false;
    if (has_osxsave) {
      uint64_t xcr0 = read_xcr0();
      os_saves_ymm = (xcr0 & 0x6) == 0x6;
      os_saves_zmm = (xcr0 & 0xE6) == 0xE6;
    }
    features.avx = has_avx_bit && os_saves_ymm;
    features.fma = has_fma && os_saves_ymm;
    if (max_leaf >= 7) {
      run_cpuid(7, 0, registers);
      unsigned ebx = registers[1];
      features.avx2 = features.avx && ((ebx >> 5) & 1);
      features.avx512f = os_saves_zmm && ((ebx >> 16) & 1);
    }
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  features.neon = true;
#elif defined(__ARM_NEON)
  features.neon = true;
#endif
  return features;
}

std::string cpu_model() {
#ifdef MAAS_X86
  unsigned registers[4] = {0, 0, 0, 0};
  run_cpuid(0x80000000u, 0, registers);
  if (registers[0] >= 0x80000004u) {
    char brand[49];
    std::memset(brand, 0, sizeof(brand));
    for (unsigned leaf = 0; leaf < 3; leaf++) {
      run_cpuid(0x80000002u + leaf, 0, registers);
      std::memcpy(brand + leaf * 16, registers, 16);
    }
    std::string text = trim(brand);
    // Collapse the runs of spaces some Intel CPUs put in their brand string.
    std::string cleaned;
    for (char character : text) {
      if (character == ' ' && !cleaned.empty() && cleaned.back() == ' ') continue;
      cleaned.push_back(character);
    }
    if (!cleaned.empty()) return cleaned;
  }
#endif
#if defined(__APPLE__)
  char buffer[256];
  size_t size = sizeof(buffer);
  if (sysctlbyname("machdep.cpu.brand_string", buffer, &size, nullptr, 0) == 0) return trim(buffer);
#elif defined(__linux__)
  std::ifstream input("/proc/cpuinfo");
  std::string line;
  std::string fallback;
  while (std::getline(input, line)) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (key == "model name" && !value.empty()) return value;
    if ((key == "Hardware" || key == "Model") && !value.empty()) fallback = value;
  }
  if (!fallback.empty()) return fallback;
#endif
  return "unknown CPU";
}

int cpu_cores() {
  unsigned count = std::thread::hardware_concurrency();
  return count == 0 ? 1 : static_cast<int>(count);
}

}  // namespace maas
