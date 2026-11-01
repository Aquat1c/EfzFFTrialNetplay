#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

constexpr std::uint32_t kHashMod = 0x8BC5D801u;

std::uint32_t rotateLeft32(std::uint32_t value, unsigned count)
{
    count &= 31u;
    return (value << count) | (value >> (32u - count));
}

std::uint32_t computeEfzHash(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t hash = 0xFFFFFFFFu;
    std::size_t pos = bytes.size();

    while (pos >= 4)
    {
        pos -= 4;
        const std::uint32_t dword = static_cast<std::uint32_t>(bytes[pos])
            | (static_cast<std::uint32_t>(bytes[pos + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[pos + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
        if (dword != 0)
        {
            hash = (rotateLeft32(hash, 5) + dword) ^ kHashMod;
        }
        if (pos == 0)
        {
            break;
        }
    }

    return hash;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "Usage: hash_efz <path-to-EFZ.EXE>\n");
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "Could not open: %s\n", argv[1]);
        return 1;
    }

    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::uint32_t hash = computeEfzHash(bytes);

    std::printf("File: %s\n", argv[1]);
    std::printf("Size: %zu bytes\n", bytes.size());
    std::printf("HASH_EFZ_EXE = 0x%08X\n", hash);
    return 0;
}
