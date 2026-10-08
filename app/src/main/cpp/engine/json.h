// Minimal JSON writer for passing structured results to Kotlin (parsed there
// with org.json, which ships with Android).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace engine {

class Json {
public:
    Json& beginObject() { sep(); s_ += '{'; first_ = true; return *this; }
    Json& endObject() { s_ += '}'; first_ = false; return *this; }
    Json& beginArray() { sep(); s_ += '['; first_ = true; return *this; }
    Json& endArray() { s_ += ']'; first_ = false; return *this; }

    Json& key(const char* k) {
        sep();
        str(k);
        s_ += ':';
        first_ = true;  // the value that follows needs no separator
        return *this;
    }

    Json& value(const std::string& v) { sep(); str(v.c_str()); return *this; }
    Json& value(const char* v) { sep(); str(v); return *this; }
    Json& value(bool v) { sep(); s_ += v ? "true" : "false"; return *this; }
    Json& value(int64_t v) { sep(); s_ += std::to_string(v); return *this; }
    Json& value(int v) { return value(static_cast<int64_t>(v)); }
    Json& value(unsigned v) { return value(static_cast<int64_t>(v)); }
    Json& value(uint64_t v) { sep(); s_ += std::to_string(v); return *this; }
    Json& value(double v) {
        sep();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.4f", v);
        s_ += buf;
        return *this;
    }

    template <typename T>
    Json& field(const char* k, T v) { key(k); return value(v); }

    const std::string& str() const { return s_; }

private:
    void sep() {
        if (!first_) s_ += ',';
        first_ = false;
    }
    void str(const char* v) {
        s_ += '"';
        for (const char* p = v; *p; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\') { s_ += '\\'; s_ += static_cast<char>(c); }
            else if (c == '\n') s_ += "\\n";
            else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); s_ += b; }
            else s_ += static_cast<char>(c);
        }
        s_ += '"';
    }

    std::string s_;
    bool first_ = true;
};

}  // namespace engine
