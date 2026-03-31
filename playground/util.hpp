#include <cstdint>
#include <stdio.h>
#include <string>
#include <vector>

static constexpr const char* BRACKET_STR = "\n\033[1;92m#===#===# PLAYGROUND OUTPUT #===#===#\033[0m\n";
static constexpr const char* ANSI_COLORS[12] = {
    // "\033[1;31m", //red
    "\033[1;32m",
    "\033[1;33m",
    "\033[1;34m",
    "\033[1;35m",
    "\033[1;36m",
    "\033[1;37m",
    // "\033[1;91m", //~bright red~
    "\033[1;92m",
    "\033[1;93m",
    "\033[1;94m",
    "\033[1;95m",
    "\033[1;96m",
    "\033[1;97m",
};

// Returns a deterministic color index from the input string
static inline size_t GetColorIndex(const char* s) {
    // Simple FNV-1a hash
    uint32_t hash = 2166136261u;
    for (; *s; ++s) {
        hash ^= static_cast<unsigned char>(*s);
        hash *= 16777619u;
    }
    return hash % (sizeof(ANSI_COLORS) / sizeof(ANSI_COLORS[0]));
}

// Prints a styled PLAYGROUND string with deterministic coloring based on label
static inline std::string StyledPlaygroundStr(const std::string& label) {
    size_t colorIdx = GetColorIndex(label.c_str());
    const char* color = ANSI_COLORS[colorIdx];
    char buf[256];
    // Ensures buffer is large enough for label + escape + formatting
    snprintf(buf, sizeof(buf), "\n%s#===#===# \"%s\" #===#===#\033[0m\n", color, label.c_str());
    return std::string(buf);
}

static inline std::vector<std::string> SplitCapitals(const char* input) {
    std::vector<std::string> result;
    if (!input || !*input)
        return result;

    size_t start = 0;
    size_t i = 0;
    bool first = true;

    while (input[i]) {
        if (std::isupper(static_cast<unsigned char>(input[i]))) {
            if (!first && i > start) {
                result.emplace_back(input + start, i - start);
                start = i;
            }
            first = false;
        }
        ++i;
    }
    // Add the last segment
    if (i > start)
        result.emplace_back(input + start, i - start);

    return result;
}

static inline std::string Join(const std::vector<std::string>& segments, const char* delimiter) {
    std::string result;
    for (size_t i = 0; i < segments.size(); ++i) {
        result += segments[i];
        if (i < segments.size() - 1)
            result += delimiter;
    }
    return result;
}

#define PlaygroundFunc(fName, body)                       \
    int fName() {                                         \
        printf("%s\n", StyledPlaygroundStr(Join(SplitCapitals(__func__), " ")).c_str()); \
        body;                                             \
        printf("%s", StyledPlaygroundStr(Join(SplitCapitals(__func__), " ")).c_str()); \
        return 0;                                         \
    }
#define CheckPlaygroundFunc(func) \
    do { \
        int ret = func(); \
        if (ret != 0) { \
            std::cerr << "Playground function " << #func << " returned non-zero value: " << ret << std::endl; \
            return ret; \
        } \
    } while (0)