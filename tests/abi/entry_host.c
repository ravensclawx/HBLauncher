#include <stdio.h>
int test_main(void);
int test_checks(void);
void hbx_test_report(int line, const char *expression);

void hbx_test_report(int line, const char *expression) {
    fprintf(stderr, "FAIL line %d: %s\n", line, expression);
}

int main(void) {
    int failures = test_main();
    printf("runtime_test: %d checks, %d failures\n", test_checks(), failures);
    return failures ? 1 : 0;
}
