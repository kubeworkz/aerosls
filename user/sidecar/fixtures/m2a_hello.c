#include <unistd.h>
int main(void) {
    static const char msg[] = "M2A-OK\n";
    long n = write(1, msg, sizeof(msg) - 1);
    _exit(n == (long)(sizeof(msg) - 1) ? 0 : 1);
}
