__attribute__((noinline))
static long long patchedMultiply(long long value) {
    return value * 5;
}

long long hotfixPatch(void *receiver, long long value) {
    (void)receiver;
    return patchedMultiply(value);
}
