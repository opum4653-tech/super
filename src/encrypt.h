#pragma once
#include <cstddef>
#include <utility>

template <size_t N, char KEY>
class XorString {
private:
    char encrypted[N];
    bool decrypted = false;

public:
    constexpr XorString(const char (&str)[N]) : encrypted{} {
        for (size_t i = 0; i < N; ++i)
            encrypted[i] = str[i] ^ KEY;
    }

    const char* decrypt() {
        if (!decrypted) {
            for (size_t i = 0; i < N; ++i)
                encrypted[i] ^= KEY;
            encrypted[N - 1] = '\0';
            decrypted = true;
        }
        return encrypted;
    }
};

#define ENC_STR(s) ([](){ \
    static XorString<sizeof(s), 0x5A> xs(s); \
    return xs.decrypt(); \
}())
