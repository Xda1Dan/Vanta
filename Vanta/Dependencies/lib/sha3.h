//
//  sha3.h
//  unix-base
//
//  Self-contained SHA-3 (Keccak-f[1600]). CommonCrypto covers MD5 and the SHA-2
//  family but has no SHA-3, and the UNC suite asks for sha3-224/256/384/512.
//
//  The state is addressed as bytes when absorbing and squeezing, which assumes
//  a little-endian host - true for the x86_64 target this builds for.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace sha3 {

    inline uint64_t rotl(uint64_t value, int count)
    {
        return (value << count) | (value >> (64 - count));
    }

    inline void permute(uint64_t state[25])
    {
        static const uint64_t kRoundConstants[24] = {
            0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
            0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
            0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
            0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
            0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
            0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
            0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
            0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
        };
        static const int kRotations[24] = {
            1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
            27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
        };
        static const int kLanes[24] = {
            10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
            15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
        };

        for (int round = 0; round < 24; round++) {
            uint64_t parity[5];
            for (int i = 0; i < 5; i++)
                parity[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^ state[i + 15] ^ state[i + 20];

            for (int i = 0; i < 5; i++) {
                const uint64_t theta = parity[(i + 4) % 5] ^ rotl(parity[(i + 1) % 5], 1);
                for (int j = 0; j < 25; j += 5)
                    state[j + i] ^= theta;
            }

            uint64_t carried = state[1];
            for (int i = 0; i < 24; i++) {
                const int lane = kLanes[i];
                const uint64_t held = state[lane];
                state[lane] = rotl(carried, kRotations[i]);
                carried = held;
            }

            for (int j = 0; j < 25; j += 5) {
                uint64_t row[5];
                for (int i = 0; i < 5; i++)
                    row[i] = state[j + i];
                for (int i = 0; i < 5; i++)
                    state[j + i] ^= (~row[(i + 1) % 5]) & row[(i + 2) % 5];
            }

            state[0] ^= kRoundConstants[round];
        }
    }

    // digestBits is 224, 256, 384 or 512. Returns the raw digest.
    inline std::string hash(const void* data, size_t length, int digestBits)
    {
        const size_t digestBytes = static_cast<size_t>(digestBits) / 8;
        const size_t rate = 200 - 2 * digestBytes;

        uint64_t state[25];
        std::memset(state, 0, sizeof(state));
        uint8_t* bytes = reinterpret_cast<uint8_t*>(state);

        const uint8_t* input = static_cast<const uint8_t*>(data);
        size_t consumed = 0;
        while (length - consumed >= rate) {
            for (size_t i = 0; i < rate; i++)
                bytes[i] ^= input[consumed + i];
            permute(state);
            consumed += rate;
        }

        // 0x06 is SHA-3's domain separation plus the leading pad bit; 0x80
        // closes the block. Keccak (as opposed to SHA-3) would use 0x01 here.
        const size_t remaining = length - consumed;
        for (size_t i = 0; i < remaining; i++)
            bytes[i] ^= input[consumed + i];
        bytes[remaining] ^= 0x06;
        bytes[rate - 1] ^= 0x80;
        permute(state);

        std::string digest(digestBytes, '\0');
        std::memcpy(&digest[0], bytes, digestBytes);
        return digest;
    }
}
