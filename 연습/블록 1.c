#include <stdio.h>
int main(void) {
    int *a = malloc(3 * sizeof(int));
    a[0] = 1; a[1] = 2; a[2] = 3;
    printf("%d\n", a[1]);
    free(a);
    return 0;
}
