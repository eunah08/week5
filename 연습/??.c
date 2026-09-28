void swap_wrong (int a, int b)      { int t = a; a = b; b = t; }      // 안 바뀜
void swap       (int *a, int *b)    { int t = *a; *a = *b; *b = t; }  // 바뀜