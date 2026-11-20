int printf(const char *format, ...);

static int sum_to(int limit) {
    int total = 0;
    int index = 1;

    while (index <= limit) {
        total += index;
        index++;
    }

    return total;
}

int main(void) {
    int even = 0;
    int odd = 0;
    int value;

    for (value = 0; value < 10; value++) {
        if (value % 2 == 0) {
            even += value;
        } else {
            odd += value;
        }
    }

    printf("sum_to_10=%d\n", sum_to(10));
    printf("even=%d odd=%d\n", even, odd);

    return 0;
}
