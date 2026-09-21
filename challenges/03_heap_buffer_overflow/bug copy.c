/*
 * Challenge 03 — Heap Buffer Overflow (심화: 동적 배열 성장 버그)
 *
 * [시나리오]
 *   자동 성장하는 정수 동적 배열 IntList (init/ensure/push/sum). 용량이 부족하면
 *   list_ensure() 가 용량을 2배로 늘리고 realloc 한다. 이 리스트로 큰 수열을
 *   만들어 합을 구한다.
 *
 * [기대 동작]
 *   0..N-1 을 100 으로 나눈 나머지를 리스트에 넣고, 길이·용량·합을 출력한 뒤 정상 종료.
 *
 * [증상]
 *   list_ensure() 가 새 용량(newcap)을 계산해 l->cap 에는 반영하지만,
 *   정작 realloc 은 "옛 용량(l->cap)" 으로 호출한다. 즉 논리 용량(cap)은 커지는데
 *   실제 버퍼는 한 세대 뒤처져, push 가 실제 버퍼 밖으로 계속 쓴다.
 *   힙 경계를 넘어 쓰면서 힙 메타데이터가 깨지거나(→ 이후 realloc/free 에서 SIGABRT)
 *   매핑되지 않은 페이지까지 밀고 나가 SIGSEGV. 크래시는 push 의 대입 지점 또는
 *   다음 realloc 에서 나지만, 원인은 ensure 의 realloc 인자다.
 *
 * [gdb 로 잡기]
 *   make gdb NAME=03_heap_buffer_overflow
 *   (gdb) run                         → 크래시(SIGSEGV) 또는 abort
 *   (gdb) bt                          → list_push 의 l->data[l->len]=x 또는 realloc 내부
 *   (gdb) frame N ; print *l           → cap 은 큰데 실제 버퍼는 그보다 작음(불일치)
 *   (gdb) print l->len  / print l->cap → len 이 실제 확보량을 넘어섰는지 확인
 *   (gdb) break list_ensure           → newcap 과 realloc 에 넘기는 크기를 대조
                                기존 용량: 4
                            [10][20][30][40]
                                    ↓
                              용량을 2배로 증가
                                    ↓
                                 새 용량: 8
                        [10][20][30][40][ ][ ][ ][ ]
 *
 * TODO: realloc 은 반드시 "새 용량(newcap)" 으로 호출하고, l->cap 갱신과 순서를 맞춰야 한다.
 *       (성장 로직은 '용량 필드'와 '실제 확보량'이 항상 같도록 유지해야 한다)
 */

#include <stdio.h>    // printf 함수를 사용하기 위한 헤더
#include <stdlib.h>   // malloc, realloc, free, exit 함수를 사용하기 위한 헤더

typedef struct {
    int   *data;      // 동적으로 할당한 배열의 시작 주소

    /* [Thinking Point]
     * 개수/크기를 담는 len, cap 을 왜 int 가 아니라 size_t 로 선언할까?
     *   tip 1. size_t 는 "이 플랫폼에서 표현 가능한 가장 큰 객체 크기"를 담도록 만든
     *          부호 없는(unsigned) 정수 타입이다. malloc/sizeof/strlen 의 타입도 size_t 다.
     *   tip 2. int 는 보통 32비트라 약 21억(2^31-1)에서 넘치고, 음수도 가능하다.
     *          원소가 그보다 많아지거나 cap*sizeof(int) 계산이 커지면 int 는 오버플로된다.
     *   생각해보기: 크기를 int 로 두면 어떤 버그가 생길 수 있을까?
     */
    size_t len;      // 현재 배열에 저장된 데이터의 개수
    size_t cap;      // 현재 배열에 저장할 수 있는 최대 데이터 개수
} IntList;            // IntList라는 구조체 이름 정의

static void list_init(IntList *l) {                  // 동적 배열을 처음 설정하는 함수
    l->cap  = 8;                                     // 배열의 최대 저장 공간을 8칸으로 설정
    l->len  = 0;                                     // 현재 저장된 데이터 개수를 0으로 설정
    l->data = malloc(l->cap * sizeof(int));          // 정수 8개를 저장할 메모리 공간을 동적 할당
    if (!l->data) { perror("malloc"); exit(1); }     // 만약 l->data가 NULL이라면 오류 출력, 프로그램 종료
}

static void list_ensure(IntList *l, size_t need) {   // 필요한 공간이 있는지 확인하고 부족하면 늘리는 함수
    if (need <= l->cap) return;                      // 필요한 공간이 현재 용량보다 작거나 같으면 함수 종료

    size_t newcap = l->cap ? l->cap * 2 : 8;         // 현재 용량이 있으면 2배로 늘리고 없으면 8로 설정
    while (newcap < need) newcap *= 2;               // 필요한 공간보다 작으면 계속 2배로 증가

    int *p = realloc(l->data, l->cap * sizeof(int)); // 기존 메모리를 다시 할당하고 새로운 주소를 p에 저장
    if (!p) { perror("realloc"); free(l->data); exit(1); }  // 메모리 재할당에 실패하면 오류 출력 후 기존 메모리 해제하고 종료

    l->data = p;                                     // 새로 할당된 메모리 주소를 data에 저장
    l->cap  = newcap;                                // 새로운 용량을 cap에 저장
}

static void list_push(IntList *l, int x) {            // 배열에 새로운 값을 추가하는 함수
    if (l->len == l->cap) list_ensure(l, l->cap + 1); // 저장 공간이 가득 차면 공간을 늘림
    l->data[l->len++] = x;                            // 현재 위치에 x를 저장하고 len을 1 증가
}

static long long list_sum(const IntList *l) {         // 배열에 저장된 모든 값의 합을 계산하는 함수
    long long s = 0;                                  // 합을 저장할 변수를 0으로 설정
    for (size_t i = 0; i < l->len; i++)               // 저장된 데이터의 개수만큼 반복
        s += l->data[i];                              // 현재 데이터를 합에 더함
    return s;                                         // 계산한 합을 반환
}

static void list_free(IntList *l) {                   // 동적으로 할당한 메모리를 해제하는 함수
    free(l->data);                                    // data가 가리키는 메모리를 해제
    l->data = NULL;                                   // 해제한 메모리를 가리키지 않도록 NULL로 설정
    l->len = l->cap = 0;                              // 데이터 개수와 용량을 0으로 설정
}

int main(void) {                                      // 프로그램 시작 함수
    IntList l;                                        // IntList 변수 l 생성
    list_init(&l);                                    // l의 배열을 초기화
    const int N = 2000000;                            // 총 200만 개의 데이터를 저장
    for (int i = 0; i < N; i++) {                     // 0부터 N-1까지 반복
        list_push(&l, i % 100);                       // i를 100으로 나눈 나머지를 배열에 추가
    }
    printf("len=%zu cap=%zu sum=%lld\n", l.len, l.cap, list_sum(&l));  // 데이터 개수, 용량, 전체 합 출력
    list_free(&l);                                    // 동적으로 할당한 메모리를 해제
    return 0;                                         // 프로그램 정상 종료
}