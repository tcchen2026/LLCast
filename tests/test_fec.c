#include "check.h"
#include "lls_fec.h"
#include "lls_net.h"
#include "llstream_proto.h"
#include <string.h>

static uint32_t state = 0x94b30a51;
static uint32_t random32(void)
{ state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }

static void trial(unsigned k, unsigned m, unsigned size, uint32_t mask, bool random_loss)
{
    size_t bytes = (size_t)(k + m) * size;
    uint8_t *original = malloc(bytes), *working = malloc(bytes);
    CHECK(original && working);
    uint8_t *shards[255], *parity[255];
    const uint8_t *data[255];
    bool present[255];
    for (unsigned i = 0; i < k + m; ++i) {
        shards[i] = working + (size_t)i * size;
        if (i < k) data[i] = original + (size_t)i * size;
        else parity[i - k] = original + (size_t)i * size;
        present[i] = true;
    }
    for (size_t i = 0; i < (size_t)k * size; ++i) original[i] = (uint8_t)random32();
    CHECK(lls_fec_encode(data, k, parity, m, size));
    memcpy(working, original, bytes);
    unsigned received = k + m;
    if (random_loss) {
        unsigned missing = random32() % (m + 3);
        if (missing > k + m) missing = k + m;
        while (missing) {
            unsigned i = random32() % (k + m);
            if (present[i]) { present[i] = false; --received; --missing; }
        }
    } else {
        for (unsigned i = 0; i < k + m; ++i)
            if (mask & (1u << i)) { present[i] = false; --received; }
    }
    for (unsigned i = 0; i < k + m; ++i)
        if (!present[i]) memset(shards[i], 0xa5, size);
    CHECK(lls_fec_decode(shards, present, k, m, size) == (received >= k));
    if (received >= k) CHECK(!memcmp(working, original, (size_t)k * size));
    for (unsigned i = k; i < k + m; ++i)
        for (unsigned j = 0; j < size; ++j)
            CHECK(shards[i][j] == (present[i] ? original[(size_t)i * size + j] : 0xa5));
    free(working); free(original);
}

static void vector(void)
{
    const uint8_t input[3][4] = {{0,1,2,3}, {4,5,6,7}, {8,9,10,11}};
    const uint8_t expected[2][4] = {{245,200,143,178}, {83,142,244,41}};
    uint8_t result[2][4];
    const uint8_t *data[] = {input[0], input[1], input[2]};
    uint8_t *parity[] = {result[0], result[1]};
    CHECK(lls_fec_encode(data, 3, parity, 2, 4));
    CHECK(!memcmp(result, expected, sizeof(result)));
    CHECK(!lls_fec_encode(data, 0, parity, 2, 4));
    CHECK(!lls_fec_encode(data, 254, parity, 2, 4));
    CHECK(!lls_fec_decode(NULL, NULL, 3, 2, 4));
    CHECK(lls_fec_parity_per_block(1, 0, 5) == 0);
    CHECK(lls_fec_parity_per_block(1, 20, 1) == 1);
    CHECK(lls_fec_parity_per_block(129, 20, 1) == 13);
    CHECK(lls_fec_parity_per_block(256, 20, 1) == 26);
    CHECK(lls_fec_parity_per_block(128, UINT32_MAX, UINT32_MAX) == 127);
    CHECK(lls_fec_parity_per_block(UINT16_MAX, 20, 1) == 0);
}

/* Release-build CPU cost only; no socket, encoder, device or display latency. */
static void benchmark(unsigned k, unsigned m, unsigned erased)
{
    enum { ITERATIONS = 200, SIZE = LLS_MAX_VIDEO_PAYLOAD };
    uint8_t *storage = calloc(k + m, SIZE); CHECK(storage);
    uint8_t *shards[255], *parity[255]; const uint8_t *data[255]; bool present[255];
    for (unsigned i = 0; i < k + m; ++i) {
        shards[i] = storage + (size_t)i * SIZE;
        if (i < k) data[i] = shards[i]; else parity[i - k] = shards[i];
        present[i] = i >= erased;
        for (unsigned j = 0; j < SIZE; ++j) shards[i][j] = (uint8_t)random32();
    }
    uint64_t start = lls_net_now_us();
    for (unsigned i = 0; i < ITERATIONS; ++i) CHECK(lls_fec_encode(data, k, parity, m, SIZE));
    double encode = (lls_net_now_us() - start) / (double)ITERATIONS;
    start = lls_net_now_us();
    for (unsigned i = 0; i < ITERATIONS; ++i) CHECK(lls_fec_decode(shards, present, k, m, SIZE));
    double decode = (lls_net_now_us() - start) / (double)ITERATIONS;
    printf("FEC CPU k=%u m=%u bytes=%u erased=%u iterations=%u encode_us=%.3f decode_us=%.3f\n",
           k, m, k * SIZE, erased, ITERATIONS, encode, decode);
    free(storage);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) {
        benchmark(37, 8, 1); benchmark(37, 8, 5); benchmark(128, 26, 5);
        benchmark(128, 26, 26);
        return 0;
    }
    vector();
    unsigned patterns = 0;
    for (unsigned k = 1; k <= 8; ++k)
        for (unsigned m = 0; m <= 4; ++m)
            for (unsigned mask = 0; mask < (1u << (k + m)); ++mask) {
                trial(k, m, 17, mask, false); ++patterns;
            }
    for (unsigned n = 0; n < 800; ++n)
        trial(1 + random32() % 128, random32() % 33, 1 + random32() % 1356, 0, true);
    trial(128, 32, 1356, 0, true);
    trial(1, 254, 31, 0, true);
    printf("FEC: %u exhaustive patterns, 802 random blocks, fixed wire vector verified\n", patterns);
}
