#include <cstddef>
#include <cstdint>

extern "C" {
int nexa_decompile(const uint8_t* bytecode, size_t length, uint8_t encodeKey,
                   uint32_t timeoutMs, char** out);
int nexa_decompile_named(const uint8_t* bytecode, size_t length, uint8_t encodeKey,
                         uint32_t timeoutMs, const uint8_t* name, size_t nameLength,
                         char** out);
void nexa_decompile_free(char* text);

int vanta_decompile(const uint8_t* bytecode, size_t length, uint8_t encodeKey,
                    uint32_t timeoutMs, char** out)
{
    return nexa_decompile(bytecode, length, encodeKey, timeoutMs, out);
}

int vanta_decompile_named(const uint8_t* bytecode, size_t length, uint8_t encodeKey,
                          uint32_t timeoutMs, const uint8_t* name, size_t nameLength,
                          char** out)
{
    return nexa_decompile_named(bytecode, length, encodeKey, timeoutMs, name, nameLength, out);
}

void vanta_decompile_free(char* text)
{
    nexa_decompile_free(text);
}
}
