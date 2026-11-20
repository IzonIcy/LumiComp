int printf(const char *format, ...);

typedef unsigned long ulong;

ulong mix(ulong seed, ulong input) {
    seed = seed * 16777619u;
    return seed ^ input;
}

int main(void) {
    ulong hash = 2166136261u;
    ulong shifted;
    int index;

    for (index = 0; index < 4; index++) {
        hash = mix(hash, (ulong)index);
    }

    shifted = hash >> 40;

    printf("hash=%lu\n", hash);
    printf("shr=%lu shl=%lu\n", shifted, shifted << 3);
    printf("div=%lu mod=%lu\n", hash / 1000000u, hash % 1000000u);
    printf("signed=%d %d\n", -9 / 2, -9 % 2);

    return 0;
}
