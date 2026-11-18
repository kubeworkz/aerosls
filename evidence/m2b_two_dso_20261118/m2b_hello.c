#include <stdio.h>

extern int m2a_data;
extern int m2a_add(int);

int main(void) {
    int v = m2a_add(m2a_data);
    puts(v == 82 ? "M2B-OK" : "M2B-BAD");
    return v == 82 ? 0 : 1;
}
