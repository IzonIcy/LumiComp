int printf(const char *format, ...);
int puts(const char *message);

int add(int left, int right) {
    return left + right;
}

int main(void) {
    int sum = add(19, 23);

    puts("hello from compiled x86-64");
    printf("sum=%d\n", sum);
    printf("answer=%d\n", sum * 2 - 42);

    return 0;
}
