int printf(const char *format, ...);

int fib(int n) {
    if (n < 2) {
        return n;
    }

    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    int index;

    for (index = 0; index < 10; index++) {
        printf("fib(%d)=%d\n", index, fib(index));
    }

    return 0;
}
