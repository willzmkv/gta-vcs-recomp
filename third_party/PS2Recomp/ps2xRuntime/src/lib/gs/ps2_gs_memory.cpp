#include <array>

#include "runtime/gs/ps2_gs_memory.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>  // [gta-recomp] libstdc++ (Linux) does not bring it in

namespace GSMem
{
    using C32Traits  = PixelStorageTraits<C32>;
    using Z32Traits  = PixelStorageTraits<Z32>;
    using C16Traits  = PixelStorageTraits<C16>;
    using C16STraits = PixelStorageTraits<C16S>;
    using Z16Traits  = PixelStorageTraits<Z16>;
    using Z16STraits = PixelStorageTraits<Z16S>;
    using P8Traits   = PixelStorageTraits<P8>;
    using P4Traits   = PixelStorageTraits<P4>;

    using C32PageLookupTable    = C32Traits::PageLookupTableT;
    using C32BlockLookupTableT  = C32Traits::BlockLookupTableT;
    using Z32PageLookupTableT = Z32Traits::PageLookupTableT;
    using Z32BlockLookupTableT = Z32Traits::BlockLookupTableT;

    // column table is shared
    using C32ColumnLookupTableT = C32Traits::ColumnLookupTableT;

    using C16PageLookupTable   = C16Traits::PageLookupTableT;
    using C16BlockLookupTable  = C16Traits::BlockLookupTableT;
    using Z16PageLookupTable   = Z16Traits::PageLookupTableT;
    using Z16BlockLookupTable  = Z16Traits::BlockLookupTableT;

    using C16SPageLookupTable  = C16STraits::PageLookupTableT;
    using C16SBlockLookupTable = C16STraits::BlockLookupTableT;
    using Z16SPageLookupTable  = Z16STraits::PageLookupTableT;
    using Z16SBlockLookupTable = Z16STraits::BlockLookupTableT;

    // column table is shared
    using C16ColumnLookupTable = C16Traits::ColumnLookupTableT;

    using P8PageLookupTable   = P8Traits::PageLookupTableT;
    using P8BlockLookupTable  = P8Traits::BlockLookupTableT;
    using P8ColumnLookupTable = P8Traits::ColumnLookupTableT;

    using P4PageLookupTable   = P4Traits::PageLookupTableT;
    using P4BlockLookupTable  = P4Traits::BlockLookupTableT;
    using P4ColumnLookupTable = P4Traits::ColumnLookupTableT;

    static constexpr C32BlockLookupTableT BlockTableC32
    {{
        {  0,  1,  4,  5, 16, 17, 20, 21 },
        {  2,  3,  6,  7, 18, 19, 22, 23 },
        {  8,  9, 12, 13, 24, 25, 28, 29 },
        { 10, 11, 14, 15, 26, 27, 30, 31 }
    }};

    static constexpr Z32BlockLookupTableT BlockTableZ32
    {{
        { 24, 25, 28, 29,  8,  9, 12, 13 },
        { 26, 27, 30, 31, 10, 11, 14, 15 },
        { 16, 17, 20, 21,  0,  1,  4,  5 },
        { 18, 19, 22, 23,  2,  3,  6,  7 }
    }};

    static constexpr C16BlockLookupTable BlockTableC16
    {{
        {  0,  2,  8, 10 },
        {  1,  3,  9, 11 },
        {  4,  6, 12, 14 },
        {  5,  7, 13, 15 },
        { 16, 18, 24, 26 },
        { 17, 19, 25, 27 },
        { 20, 22, 28, 30 },
        { 21, 23, 29, 31 }
    }};

    static constexpr C16SBlockLookupTable BlockTableC16S
    {{
        {  0,  2, 16, 18 },
        {  1,  3, 17, 19 },
        {  8, 10, 24, 26 },
        {  9, 11, 25, 27 },
        {  4,  6, 20, 22 },
        {  5,  7, 21, 23 },
        { 12, 14, 28, 30 },
        { 13, 15, 29, 31 }
    }};

    static constexpr Z16BlockLookupTable BlockTableZ16
    {{
        { 24, 26, 16, 18 },
        { 25, 27, 17, 19 },
        { 28, 30, 20, 22 },
        { 29, 31, 21, 23 },
        {  8, 10,  0,  2 },
        {  9, 11,  1,  3 },
        { 12, 14,  4,  6 },
        { 13, 15,  5,  7 }
    }};

    static constexpr Z16SBlockLookupTable BlockTableZ16S
    {{
        { 24, 26,  8, 10 },
        { 25, 27,  9, 11 },
        { 16, 18,  0,  2 },
        { 17, 19,  1,  3 },
        { 28, 30, 12, 14 },
        { 29, 31, 13, 15 },
        { 20, 22,  4,  6 },
        { 21, 23,  5,  7 }
    }};

    static constexpr P8BlockLookupTable BlockTableP8
    {{
        {  0,  1,  4,  5, 16, 17, 20, 21},
        {  2,  3,  6,  7, 18, 19, 22, 23},
        {  8,  9, 12, 13, 24, 25, 28, 29},
        { 10, 11, 14, 15, 26, 27, 30, 31}
    }};

    static constexpr P4BlockLookupTable BlockTableP4
    {{
        {  0,  2,  8, 10 },
        {  1,  3,  9, 11 },
        {  4,  6, 12, 14 },
        {  5,  7, 13, 15 },
        { 16, 18, 24, 26 },
        { 17, 19, 25, 27 },
        { 20, 22, 28, 30 },
        { 21, 23, 29, 31 }
    }};

    // column layout is shared between c32 and z32
    static constexpr C32ColumnLookupTableT ColumnTable32
    {{
        {  0,  1,  4,  5,  8,  9, 12, 13 },
        {  2,  3,  6,  7, 10, 11, 14, 15 },
        { 16, 17, 20, 21, 24, 25, 28, 29 },
        { 18, 19, 22, 23, 26, 27, 30, 31 },
        { 32, 33, 36, 37, 40, 41, 44, 45 },
        { 34, 35, 38, 39, 42, 43, 46, 47 },
        { 48, 49, 52, 53, 56, 57, 60, 61 },
        { 50, 51, 54, 55, 58, 59, 62, 63 },
    }};

    static constexpr C16ColumnLookupTable ColumnTable16
    {{
        {   0,   2,   8,  10,  16,  18,  24,  26,   1,   3,   9,  11,  17,  19,  25,  27 },
        {   4,   6,  12,  14,  20,  22,  28,  30,   5,   7,  13,  15,  21,  23,  29,  31 },
        {  32,  34,  40,  42,  48,  50,  56,  58,  33,  35,  41,  43,  49,  51,  57,  59 },
        {  36,  38,  44,  46,  52,  54,  60,  62,  37,  39,  45,  47,  53,  55,  61,  63 },
        {  64,  66,  72,  74,  80,  82,  88,  90,  65,  67,  73,  75,  81,  83,  89,  91 },
        {  68,  70,  76,  78,  84,  86,  92,  94,  69,  71,  77,  79,  85,  87,  93,  95 },
        {  96,  98, 104, 106, 112, 114, 120, 122,  97,  99, 105, 107, 113, 115, 121, 123 },
        { 100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127 }
    }};

    static constexpr P8ColumnLookupTable ColumnTable8
    {{
        {   0,   4,  16,  20,  32,  36,  48,  52,   2,   6,  18,  22,  34,  38,  50,  54 },
        {   8,  12,  24,  28,  40,  44,  56,  60,  10,  14,  26,  30,  42,  46,  58,  62 },
        {  33,  37,  49,  53,   1,   5,  17,  21,  35,  39,  51,  55,   3,   7,  19,  23 },
        {  41,  45,  57,  61,   9,  13,  25,  29,  43,  47,  59,  63,  11,  15,  27,  31 },
        {  96, 100, 112, 116,  64,  68,  80,  84,  98, 102, 114, 118,  66,  70,  82,  86 },
        { 104, 108, 120, 124,  72,  76,  88,  92, 106, 110, 122, 126,  74,  78,  90,  94 },
        {  65,  69,  81,  85,  97, 101, 113, 117,  67,  71,  83,  87,  99, 103, 115, 119 },
        {  73,  77,  89,  93, 105, 109, 121, 125,  75,  79,  91,  95, 107, 111, 123, 127 },
        { 128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182 },
        { 136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190 },
        { 161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151 },
        { 169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159 },
        { 224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214 },
        { 232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222 },
        { 193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247 },
        { 201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255 }
    }};

    static constexpr P4ColumnLookupTable ColumnTable4
    {{
        {   0,   8,  32,  40,  64,  72,  96, 104,   2,  10,  34,  42,  66,  74,  98, 106,   4,  12,  36,  44,  68,  76, 100, 108,   6,  14,  38,  46,  70,  78, 102, 110 },
        {  16,  24,  48,  56,  80,  88, 112, 120,  18,  26,  50,  58,  82,  90, 114, 122,  20,  28,  52,  60,  84,  92, 116, 124,  22,  30,  54,  62,  86,  94, 118, 126 },
        {  65,  73,  97, 105,   1,   9,  33,  41,  67,  75,  99, 107,   3,  11,  35,  43,  69,  77, 101, 109,   5,  13,  37,  45,  71,  79, 103, 111,   7,  15,  39,  47 },
        {  81,  89, 113, 121,  17,  25,  49,  57,  83,  91, 115, 123,  19,  27,  51,  59,  85,  93, 117, 125,  21,  29,  53,  61,  87,  95, 119, 127,  23,  31,  55,  63 },
        { 192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174 },
        { 208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190 },
        { 129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239 },
        { 145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255 },
        { 256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366 },
        { 272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382 },
        { 321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303 },
        { 337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319 },
        { 448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430 },
        { 464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446 },
        { 385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495 },
        { 401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511 },
    }};

    // this is going to be massive (an entire page of addess lookups)
    static C32PageLookupTable  PageTableC32{ };
    static Z32PageLookupTableT PageTableZ32{ };
    static C16PageLookupTable  PageTableC16{ };
    static C16SPageLookupTable PageTableC16S{ };
    static Z16PageLookupTable  PageTableZ16{ };
    static Z16SPageLookupTable PageTableZ16S{ };
    static P8PageLookupTable   PageTableP8{ };
    static P4PageLookupTable   PageTableP4{ };

    void InitLookupTables()
    {
        // 32 bit
        PixelStorageTraits<C32>::InitPageLookupTable(PageTableC32, BlockTableC32, ColumnTable32);
        PixelStorageTraits<Z32>::InitPageLookupTable(PageTableZ32, BlockTableZ32, ColumnTable32);

        // 16 bit
        PixelStorageTraits<C16>::InitPageLookupTable(PageTableC16, BlockTableC16, ColumnTable16);
        PixelStorageTraits<C16S>::InitPageLookupTable(PageTableC16S, BlockTableC16S, ColumnTable16);
        PixelStorageTraits<Z16>::InitPageLookupTable(PageTableZ16, BlockTableZ16, ColumnTable16);
        PixelStorageTraits<Z16S>::InitPageLookupTable(PageTableZ16S, BlockTableZ16S, ColumnTable16);

        // 8 bit
        PixelStorageTraits<P8>::InitPageLookupTable(PageTableP8, BlockTableP8, ColumnTable8);

        // 4 bit
        PixelStorageTraits<P4>::InitPageLookupTable(PageTableP4, BlockTableP4, ColumnTable4);
    }

    u32 ReadTexture(TexturePageCache& cache, const u8* data, u32 psm, u32 bp, u32 bw, u32 x, u32 y)
    {
        switch (static_cast<PixelStorageMode>(psm & 0x3Fu))
        {
        case C32:
            return PixelStorageTraits<C32>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case C24:
            return PixelStorageTraits<C24>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case C16:
            return PixelStorageTraits<C16>::Read(PageTableC16, data, bp, bw, x, y, &cache);
        case C16S:
            return PixelStorageTraits<C16S>::Read(PageTableC16S, data, bp, bw, x, y, &cache);
        case P8:
            return PixelStorageTraits<P8>::Read(PageTableP8, data, bp, bw, x, y, &cache);
        case P4:
            return PixelStorageTraits<P4>::Read(PageTableP4, data, bp, bw, x, y, &cache);
        case P8H:
            return PixelStorageTraits<P8H>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case P4HL:
            return PixelStorageTraits<P4HL>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case P4HH:
            return PixelStorageTraits<P4HH>::Read(PageTableC32, data, bp, bw, x, y, &cache);
        case Z32:
            return PixelStorageTraits<Z32>::Read(PageTableZ32, data, bp, bw, x, y, &cache);
        case Z24:
            return PixelStorageTraits<Z24>::Read(PageTableZ32, data, bp, bw, x, y, &cache);
        case Z16:
            return PixelStorageTraits<Z16>::Read(PageTableZ16, data, bp, bw, x, y, &cache);
        case Z16S:
            return PixelStorageTraits<Z16S>::Read(PageTableZ16S, data, bp, bw, x, y, &cache);
        default:
            return 0u;
        }
    }

    namespace // [gta-recomp] PixelBits
    {
        template <PixelStorageMode psm, typename Table>
        void pixelBitsOf(const Table& table, u32 bp, u32 bw, u32 x, u32 y, u32& bit, u32& width)
        {
            const uint64_t bits = static_cast<uint64_t>(PixelStorageTraits<psm>::Address(table, bp, bw, x, y)) * UnpackedBitWidth(psm) +
                             PixelStorageTraits<psm>::BitOffset();
            bit = static_cast<u32>(bits % (static_cast<uint64_t>(MEMORY_SIZE) * 8u));
            width = static_cast<u32>(BitsPerPixel(psm));
        }
    }

    bool PixelBits(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32& bit, u32& width)
    {
        switch (static_cast<PixelStorageMode>(psm & 0x3Fu))
        {
        case C32: pixelBitsOf<C32>(PageTableC32, bp, bw, x, y, bit, width); return true;
        case C24: pixelBitsOf<C24>(PageTableC32, bp, bw, x, y, bit, width); return true;
        case C16: pixelBitsOf<C16>(PageTableC16, bp, bw, x, y, bit, width); return true;
        case C16S: pixelBitsOf<C16S>(PageTableC16S, bp, bw, x, y, bit, width); return true;
        case P8: pixelBitsOf<P8>(PageTableP8, bp, bw, x, y, bit, width); return true;
        case P4: pixelBitsOf<P4>(PageTableP4, bp, bw, x, y, bit, width); return true;
        case P8H: pixelBitsOf<P8H>(PageTableC32, bp, bw, x, y, bit, width); return true;
        case P4HL: pixelBitsOf<P4HL>(PageTableC32, bp, bw, x, y, bit, width); return true;
        case P4HH: pixelBitsOf<P4HH>(PageTableC32, bp, bw, x, y, bit, width); return true;
        default: return false;
        }
    }

    void WriteCT32(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C32>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteCT24(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C24>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteZ32(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z32>::Write(PageTableZ32, data, bp, bw, x, y, value);
    }

    void WriteZ24(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z24>::Write(PageTableZ32, data, bp, bw, x, y, value);
    }

    void WriteCT16(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C16>::Write(PageTableC16, data, bp, bw, x, y, value);
    }

    void WriteCT16S(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<C16S>::Write(PageTableC16S, data, bp, bw, x, y, value);
    }

    void WriteZ16(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z16>::Write(PageTableZ16, data, bp, bw, x, y, value);
    }

    void WriteZ16S(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<Z16S>::Write(PageTableZ16S, data, bp, bw, x, y, value);
    }

    void WriteP8(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P8>::Write(PageTableP8, data, bp, bw, x, y, value);
    }

    void WriteP8H(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P8H>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteP4(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P4>::Write(PageTableP4, data, bp, bw, x, y, value);
    }

    void WriteP4HL(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P4HL>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteP4HH(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
        PixelStorageTraits<P4HH>::Write(PageTableC32, data, bp, bw, x, y, value);
    }

    void WriteNull(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 value)
    {
    }

    // [gta-recomp] Whole-rectangle uploads, see the header. The page table is linear in the base block:
    // table[block][y][x] == block * PixelsPerBlock + table[0][y][x], so slice 0 is enough.
    namespace
    {
        template <PixelStorageMode psm>
        u32 sliceAddress(const typename PixelStorageTraits<psm>::PageLookupTableT& table, u32 bp, u32 bw, u32 x, u32 y)
        {
            using T = PixelStorageTraits<psm>;
            constexpr auto pe = T::PageExtent();
            return static_cast<u32>(T::PageId(bp, bw, x, y)) * static_cast<u32>(T::PixelsPerPage()) +
                   (bp % static_cast<u32>(T::BlocksPerPage())) * static_cast<u32>(T::PixelsPerBlock()) + table[0][y % pe.y][x % pe.x];
        }

        // Compares sliceAddress with Address on pseudo-random inputs once.
        template <PixelStorageMode psm>
        bool sliceMatches(const typename PixelStorageTraits<psm>::PageLookupTableT& table)
        {
            u32 seed = 0x12345678u;
            auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
            for (int i = 0; i < 20000; ++i)
            {
                const u32 bp = next() % 16384u, bw = 1u + next() % 16u, x = next() % 1024u, y = next() % 1024u;
                if (sliceAddress<psm>(table, bp, bw, x, y) != PixelStorageTraits<psm>::Address(table, bp, bw, x, y))
                {
                    std::fprintf(stderr, "[gs] rect upload self-check failed for psm 0x%x (bp=%u bw=%u x=%u y=%u): per-pixel path kept\n",
                                 static_cast<unsigned>(psm), bp, bw, x, y);
                    return false;
                }
            }
            return true;
        }

        template <PixelStorageMode psm, typename Fetch>
        void writeRectFast(const typename PixelStorageTraits<psm>::PageLookupTableT& table, u8* data, u32 bp, u32 bw, u32 x0, u32 y0,
                           u32 w, u32 h, Fetch fetch);

        template <PixelStorageMode psm, typename Fetch>
        bool writeRect(const typename PixelStorageTraits<psm>::PageLookupTableT& table, u8* data, u32 bp, u32 bw, u32 x0, u32 y0, u32 w,
                       u32 h, Fetch fetch)
        {
            static const bool ok = sliceMatches<psm>(table);
            if (!ok)
                return false;
            // PS2X_GS_RECT_VERIFY=1: every 64th upload also written pixel by pixel (Write) into a copy and compared
            static const bool verify = std::getenv("PS2X_GS_RECT_VERIFY") != nullptr;
            static u32 uploads = 0u;
            if (verify && (uploads++ & 63u) == 0u)
            {
                std::vector<u8> expected(data, data + MEMORY_SIZE);
                u32 j = 0;
                for (u32 y = y0; y < y0 + h; ++y)
                    for (u32 x = x0; x < x0 + w; ++x, ++j)
                        PixelStorageTraits<psm>::Write(table, expected.data(), bp, bw, x, y,
                                                       static_cast<typename PixelStorageTraits<psm>::PackedT>(fetch(j)));
                writeRectFast<psm>(table, data, bp, bw, x0, y0, w, h, fetch);
                static u32 checked = 0u, differing = 0u;
                ++checked;
                if (std::memcmp(expected.data(), data, MEMORY_SIZE) != 0 && differing++ < 20u)
                    std::fprintf(stderr, "[gs-rect-verify] psm 0x%x bp %u bw %u %ux%u at %u,%u differs\n", static_cast<unsigned>(psm), bp,
                                 bw, w, h, x0, y0);
                if ((checked & 255u) == 0u)
                    std::fprintf(stderr, "[gs-rect-verify] psm 0x%x: %u checked, %u differ\n", static_cast<unsigned>(psm), checked, differing);
                return true;
            }
            writeRectFast<psm>(table, data, bp, bw, x0, y0, w, h, fetch);
            return true;
        }

        // Where each pixel of a block lies inside it: the same for every block of a page (checked), so a block the upload
        // covers completely is written byte by byte from its two pixels - no per-pixel addressing, no read-modify-write.
        struct BlockLayout
        {
            bool ok = false;
            u32 origin = 0;             // offset of the block's pixel (0, 0)
            std::vector<u8> x, y;       // pixel of each offset in the block
        };

        template <PixelStorageMode psm>
        BlockLayout blockLayout(const typename PixelStorageTraits<psm>::PageLookupTableT& table)
        {
            using T = PixelStorageTraits<psm>;
            constexpr auto pe = T::PageExtent();
            constexpr auto be = T::BlockExtent();
            constexpr u32 bwPx = pe.x / be.x, bhPx = pe.y / be.y, ppb = static_cast<u32>(T::PixelsPerBlock());
            BlockLayout layout;
            layout.x.assign(ppb, 0u);
            layout.y.assign(ppb, 0u);
            std::vector<u32> offsets(ppb);
            for (u32 by = 0; by < pe.y; by += bhPx)
                for (u32 bx = 0; bx < pe.x; bx += bwPx)
                {
                    u32 lowest = ~0u;
                    for (u32 yl = 0; yl < bhPx; ++yl)
                        for (u32 xl = 0; xl < bwPx; ++xl)
                            lowest = std::min<u32>(lowest, table[0][by + yl][bx + xl]);
                    if (lowest % ppb != 0u)
                        return layout;
                    std::vector<u8> seen(ppb, 0u);
                    for (u32 yl = 0; yl < bhPx; ++yl)
                        for (u32 xl = 0; xl < bwPx; ++xl)
                        {
                            const u32 offset = table[0][by + yl][bx + xl] - lowest;
                            if (offset >= ppb || seen[offset]++)
                                return layout;
                            if (bx == 0u && by == 0u)
                            {
                                layout.x[offset] = static_cast<u8>(xl);
                                layout.y[offset] = static_cast<u8>(yl);
                                offsets[yl * bwPx + xl] = offset;
                            }
                            else if (offsets[yl * bwPx + xl] != offset)
                                return layout;
                        }
                }
            layout.origin = offsets[0];
            layout.ok = true;
            return layout;
        }

        template <PixelStorageMode psm, typename Fetch>
        void writeRectFast(const typename PixelStorageTraits<psm>::PageLookupTableT& table, u8* data, u32 bp, u32 bw, u32 x0, u32 y0,
                           u32 w, u32 h, Fetch fetch)
        {
            using T = PixelStorageTraits<psm>;
            constexpr auto pe = T::PageExtent();
            constexpr u32 ppp = static_cast<u32>(T::PixelsPerPage());
            const u32 blockBase = (bp % static_cast<u32>(T::BlocksPerPage())) * static_cast<u32>(T::PixelsPerBlock());
            if constexpr (psm == P4)
            {
                // Whole blocks at once when no two pixels of the rectangle share an address (the rectangle fits the
                // buffer width and spans less than GS memory): then the order of the writes doesn't matter.
                static const BlockLayout layout = [&] {
                    BlockLayout built = blockLayout<P4>(table);
                    if (!built.ok)
                        std::fprintf(stderr, "[gs] PSMT4 block layout self-check failed: uploads written pixel by pixel\n");
                    return built;
                }();
                constexpr auto be = T::BlockExtent();
                constexpr u32 bwPx = pe.x / be.x, bhPx = pe.y / be.y;
                const u32 pagesPerRow = (bw * 64u) / pe.x;
                const u32 pageRows = (y0 + h - 1u) / pe.y - y0 / pe.y + 1u;
                if (layout.ok && pagesPerRow != 0u && (x0 + w + pe.x - 1u) / pe.x <= pagesPerRow &&
                    (static_cast<uint64_t>(pageRows) * pagesPerRow + 1u) * ppp / 2u <= MEMORY_SIZE)
                {
                    const u32 xEnd = x0 + w, yEnd = y0 + h;
                    u32 rowStart[bhPx];
                    for (u32 by = y0 - y0 % bhPx; by < yEnd; by += bhPx)
                        for (u32 bx = x0 - x0 % bwPx; bx < xEnd; bx += bwPx)
                        {
                            if (bx >= x0 && by >= y0 && bx + bwPx <= xEnd && by + bhPx <= yEnd)
                            {
                                const u32 blockAddr = static_cast<u32>(T::PageId(bp, bw, bx, by)) * ppp + blockBase +
                                                      table[0][by % pe.y][bx % pe.x] - layout.origin;
                                u8* out = data + ((blockAddr >> 1) & (MEMORY_SIZE - 1u));
                                for (u32 yl = 0; yl < bhPx; ++yl)
                                    rowStart[yl] = (by + yl - y0) * w + (bx - x0);
                                for (u32 k = 0; k < bwPx * bhPx / 2u; ++k)
                                {
                                    const u32 lo = fetch(rowStart[layout.y[2u * k]] + layout.x[2u * k]) & 0x0Fu;
                                    const u32 hi = fetch(rowStart[layout.y[2u * k + 1u]] + layout.x[2u * k + 1u]) & 0x0Fu;
                                    out[k] = static_cast<u8>(lo | (hi << 4u));
                                }
                                continue;
                            }
                            // an edge block: its pixels inside the rectangle one by one
                            for (u32 y = std::max(by, y0); y < std::min(by + bhPx, yEnd); ++y)
                                for (u32 x = std::max(bx, x0); x < std::min(bx + bwPx, xEnd); ++x)
                                {
                                    const u32 addr = static_cast<u32>(T::PageId(bp, bw, x, y)) * ppp + blockBase + table[0][y % pe.y][x % pe.x];
                                    u8& byte = data[(addr >> 1) & (MEMORY_SIZE - 1u)];
                                    const u32 shift = (addr & 1u) * 4u;
                                    byte = static_cast<u8>((byte & ~(0x0Fu << shift)) | ((fetch((y - y0) * w + (x - x0)) & 0x0Fu) << shift));
                                }
                        }
                    return;
                }
            }
            u32 i = 0;
            for (u32 y = y0; y < y0 + h; ++y)
            {
                const auto& row = table[0][y % pe.y];
                // sliceAddress with the page's row and column hoisted: PageId(bp, bw, x, y) is PageId(bp, bw, 0, y) plus x / pe.x
                const u32 rowBase = static_cast<u32>(T::PageId(bp, bw, 0u, y)) * ppp + blockBase;
                for (u32 x = x0; x < x0 + w;)
                {
                    const u32 pageEnd = std::min(x0 + w, (x / static_cast<u32>(pe.x) + 1u) * static_cast<u32>(pe.x));
                    const u32 base = rowBase + (x / static_cast<u32>(pe.x)) * ppp;
                    for (; x < pageEnd; ++x, ++i)
                    {
                        const u32 addr = base + row[x % pe.x];
                        if constexpr (psm == P4)
                        {
                            // WriteAddress for 4-bit pixels: the nibble at addr / 2, low first
                            u8& byte = data[(addr >> 1) & (MEMORY_SIZE - 1u)];
                            const u32 shift = (addr & 1u) * 4u;
                            byte = static_cast<u8>((byte & ~(0x0Fu << shift)) | ((fetch(i) & 0x0Fu) << shift));
                        }
                        else
                            T::WriteAddress(data, addr, static_cast<typename T::PackedT>(fetch(i)));
                    }
                }
            }
        }
    }

    namespace
    {
        template <PixelStorageMode psm>
        void rectBlocks(const typename PixelStorageTraits<psm>::PageLookupTableT& table, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h,
                        std::vector<u16>& touched, std::vector<u16>* full)
        {
            using T = PixelStorageTraits<psm>;
            constexpr auto pe = T::PageExtent();
            constexpr auto be = T::BlockExtent();
            constexpr u32 bwPx = pe.x / be.x, bhPx = pe.y / be.y; // block size in pixels
            const u32 x0 = x - x % bwPx, y0 = y - y % bhPx;
            for (u32 by = y0; by < y + h; by += bhPx)
                for (u32 bx = x0; bx < x + w; bx += bwPx)
                {
                    const u32 pixel = PixelStorageTraits<psm>::Address(table, bp, bw, bx, by);
                    const u32 byte = static_cast<u32>((static_cast<uint64_t>(pixel) * UnpackedBitWidth(psm)) / 8u) & (MEMORY_SIZE - 1u);
                    const u16 block = static_cast<u16>(byte >> 8);
                    touched.push_back(block);
                    if (full && bx >= x && by >= y && bx + bwPx <= x + w && by + bhPx <= y + h)
                        full->push_back(block);
                }
        }
    }

    bool RectBlocks(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h, std::vector<u16>& touched, std::vector<u16>* full)
    {
        if (w == 0u || h == 0u)
            return true;
        bw = bw ? bw : 1u;
        switch (static_cast<PixelStorageMode>(psm & 0x3Fu))
        {
        case C32: rectBlocks<C32>(PageTableC32, bp, bw, x, y, w, h, touched, full); return true;
        case C16: rectBlocks<C16>(PageTableC16, bp, bw, x, y, w, h, touched, full); return true;
        case P8: rectBlocks<P8>(PageTableP8, bp, bw, x, y, w, h, touched, full); return true;
        case P4: rectBlocks<P4>(PageTableP4, bp, bw, x, y, w, h, touched, full); return true;
        default: return false;
        }
    }

    bool WriteRectCT32(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h, const u8* src)
    {
        return writeRect<C32>(PageTableC32, data, bp, bw, x, y, w, h, [src](u32 i) { u32 v; std::memcpy(&v, src + i * 4u, 4); return v; });
    }

    bool WriteRectCT16(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h, const u8* src)
    {
        return writeRect<C16>(PageTableC16, data, bp, bw, x, y, w, h, [src](u32 i) { u16 v; std::memcpy(&v, src + i * 2u, 2); return v; });
    }

    bool WriteRectP8(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h, const u8* src)
    {
        return writeRect<P8>(PageTableP8, data, bp, bw, x, y, w, h, [src](u32 i) { return src[i]; });
    }

    bool WriteRectP4(u8* data, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h, const u8* src)
    {
        return writeRect<P4>(PageTableP4, data, bp, bw, x, y, w, h, [src](u32 i) { return static_cast<u8>((src[i >> 1] >> ((i & 1u) * 4u)) & 0x0Fu); });
    }

    namespace
    {
        inline u32 applyTexaRect(u8 psm, u32 texel, u8 ta0, u8 ta1, u8 aem)
        {
            if (psm == 0x00u /* GS_PSM_CT32 */ || psm == 0x30u /* GS_PSM_Z32 */)
                return texel;
            const bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
            u8 a = static_cast<u8>((texel >> 24) & 0xFFu);
            if (psm == 0x01u /* GS_PSM_CT24 */)
                a = (aem && rgbZero) ? 0u : ta0;
            else if (psm == 0x02u /* GS_PSM_CT16 */ || psm == 0x0Au /* GS_PSM_CT16S */)
                a = (a & 0x80u) ? ta1 : ((aem && rgbZero) ? 0u : ta0);
            return (texel & 0x00FFFFFFu) | (static_cast<u32>(a) << 24);
        }

        template <PixelStorageMode psm>
        bool readRectFast(const typename PixelStorageTraits<psm>::PageLookupTableT& table, u32 rawPsm, const u8* data, size_t dataBytes,
                          bool wrapVram, u32 bp, u32 bw, u32 w, u32 h, const u32* palette, u8 ta0, u8 ta1, u8 aem, u32* out)
        {
            static const bool ok = sliceMatches<psm>(table);
            if (!ok || w == 0u || h == 0u)
                return ok;
            using T = PixelStorageTraits<psm>;
            constexpr auto pe = T::PageExtent();
            constexpr u32 ppp = static_cast<u32>(T::PixelsPerPage());
            const u32 blockMod = bp % static_cast<u32>(T::BlocksPerPage());
            const u32 blockBase = blockMod * static_cast<u32>(T::PixelsPerBlock());
            constexpr size_t elemSize = sizeof(typename T::PackedT);
            constexpr u32 wrapMask = static_cast<u32>(MEMORY_SIZE - elemSize);
            if (!wrapVram)
            {
                const u32 maxPage = static_cast<u32>(T::PageId(bp, bw, w - 1u, h - 1u));
                const u32 pagesNeeded = maxPage + (blockMod ? 2u : 1u);
                if (static_cast<uint64_t>(pagesNeeded) * GS_PAGE_SIZE > dataBytes)
                    return false;
            }
            else if (dataBytes < MEMORY_SIZE)
                return false;

            const u32 pageStride = static_cast<u32>(T::PageId(0u, bw, 0u, pe.y));
            const u32 rowBase0 = static_cast<u32>(T::PageId(bp, bw, 0u, 0u)) * ppp + blockBase;
            u32 dst = 0u;
            for (u32 y = 0u; y < h; ++y)
            {
                const auto& row = table[0][y % pe.y];
                const u32 rowBase = rowBase0 + (y / pe.y) * pageStride * ppp;
                for (u32 x = 0u; x < w;)
                {
                    const u32 pageEnd = std::min(w, (x / static_cast<u32>(pe.x) + 1u) * static_cast<u32>(pe.x));
                    const u32 base = rowBase + (x / static_cast<u32>(pe.x)) * ppp;
                    for (; x < pageEnd; ++x, ++dst)
                    {
                        const u32 pixelAddr = base + row[x % pe.x];
                        const u32 bits = pixelAddr * static_cast<u32>(UnpackedBitWidth(psm)) + static_cast<u32>(T::BitOffset());
                        const u32 byteAddr = wrapVram ? ((bits >> 3) & wrapMask) : (bits >> 3);
                        const u32 shift = bits & 7u;
                        typename T::PackedT v;
                        std::memcpy(&v, data + byteAddr, elemSize);
                        if constexpr (psm == C32 || psm == Z32)
                            out[dst] = v;
                        else if constexpr (psm == C24 || psm == Z24)
                            out[dst] = applyTexaRect(rawPsm, v & 0x00FFFFFFu, ta0, ta1, aem);
                        else if constexpr (psm == C16 || psm == C16S || psm == Z16 || psm == Z16S)
                        {
                            const u32 r = ((v >> 0) & 0x1Fu) << 3;
                            const u32 g = ((v >> 5) & 0x1Fu) << 3;
                            const u32 b = ((v >> 10) & 0x1Fu) << 3;
                            const u32 a = ((v >> 15) & 0x01u) << 7;
                            out[dst] = applyTexaRect(rawPsm, r | (g << 8) | (b << 16) | (a << 24), ta0, ta1, aem);
                        }
                        else if constexpr (psm == P8 || psm == P8H)
                            out[dst] = palette[v & 0xFFu];
                        else
                            out[dst] = palette[(v >> shift) & 0x0Fu];
                    }
                }
            }
            return true;
        }
    }

    bool ReadRectRGBA(u32 psm, u32 bp, u32 bw, u32 w, u32 h, const u8* data, size_t dataBytes,
                      bool wrapVram, const u32* palette, u8 ta0, u8 ta1, u8 aem, u32* out)
    {
        if (!data || !out)
            return false;
        const u32 rawPsm = psm & 0x3Fu;
        switch (static_cast<PixelStorageMode>(rawPsm))
        {
        case C32: return readRectFast<C32>(PageTableC32, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case C24: return readRectFast<C24>(PageTableC32, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case C16: return readRectFast<C16>(PageTableC16, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case C16S: return readRectFast<C16S>(PageTableC16S, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case P8: return palette && readRectFast<P8>(PageTableP8, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case P4: return palette && readRectFast<P4>(PageTableP4, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case P8H: return palette && readRectFast<P8H>(PageTableC32, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case P4HL: return palette && readRectFast<P4HL>(PageTableC32, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case P4HH: return palette && readRectFast<P4HH>(PageTableC32, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case Z32: return readRectFast<Z32>(PageTableZ32, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case Z24: return readRectFast<Z24>(PageTableZ32, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case Z16: return readRectFast<Z16>(PageTableZ16, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        case Z16S: return readRectFast<Z16S>(PageTableZ16S, rawPsm, data, dataBytes, wrapVram, bp, bw, w, h, palette, ta0, ta1, aem, out);
        default: return false;
        }
    }

    u32 ReadCT32(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C32>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadZ32(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z32>::Read(PageTableZ32, data, bp, bw, x, y);
    }

    u32 ReadCT24(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C24>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadZ24(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z24>::Read(PageTableZ32, data, bp, bw, x, y);
    }

    u32 ReadCT16(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C16>::Read(PageTableC16, data, bp, bw, x, y);
    }

    u32 ReadCT16S(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<C16S>::Read(PageTableC16S, data, bp, bw, x, y);
    }

    u32 ReadZ16(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z16>::Read(PageTableZ16, data, bp, bw, x, y);
    }

    u32 ReadZ16S(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<Z16S>::Read(PageTableZ16S, data, bp, bw, x, y);
    }

    u32 ReadP8(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P8>::Read(PageTableP8, data, bp, bw, x, y);
    }

    u32 ReadP8H(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P8H>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadP4(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P4>::Read(PageTableP4, data, bp, bw, x, y);
    }

    u32 ReadP4HL(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P4HL>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadP4HH(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return PixelStorageTraits<P4HH>::Read(PageTableC32, data, bp, bw, x, y);
    }

    u32 ReadNull(u8* data, u32 bp, u32 bw, u32 x, u32 y)
    {
        return 0;
    }
}


