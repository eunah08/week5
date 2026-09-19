/*
 * Challenge 01 — Use After Free (심화: vtable 기반 위젯 시스템)
 *
 * [시나리오]
 *   아주 작은 GUI 흉내. 각 위젯(Widget)은 힙 객체이며 첫 멤버로 "vtable"
 *   (render/on_event 함수 포인터 묶음)을 가진다. Screen 은 위젯 포인터 배열을
 *   들고 있고, 이벤트를 나눠준 뒤(dispatch) 한 프레임을 그린다(render).
 *
 * [기대 동작]
 *   버튼/라벨/다이얼로그를 그리고, 닫기 이벤트 후 남은 위젯만 다시 그린 뒤
 *   정상 종료(0).
 *
 * [증상]
 *   닫기 이벤트 핸들러가 다이얼로그 위젯을 free() 하지만, Screen 의 포인터 배열에서
 *   그 슬롯을 제거(NULL 로)하지 않는다. 그 사이 앱이 상태 메시지 버퍼를 새로 할당하며
 *   방금 해제된 청크를 재사용해 vtable 포인터 자리를 덮어쓴다.
 *   다음 렌더 패스에서 해제된 위젯의 w->vtbl->render 를 호출 → 망가진 함수 포인터로
 *   점프 → SIGSEGV. 크래시는 render 루프에서 나지만, 원인은 멀리 떨어진 close 핸들러다.
 *
 * [gdb 로 잡기]
 *   make gdb NAME=01_use_after_free
 *   (gdb) run                         → 크래시(SIGSEGV)
 *   (gdb) bt                          → screen_render() 안 w->vtbl->render(w) 지점
 *   (gdb) print w                     → 어떤 위젯인지(주소/슬롯) 확인
 *   (gdb) print w->vtbl               → 오염돼 있음
 *   (gdb) print s->items[2]           → 이미 해제된 슬롯이 그대로 남아있음
 *   (gdb) break widget_destroy        → 누가/언제 이 위젯을 free 하는지 역추적
 *
 * [printf(로그)로 잡기]
 *   위젯 해제 시점과 렌더 시점의 vtbl 값을 각각 찍어 "해제가 사용보다 먼저"인지 확인:
 *     (destroy) fprintf(stderr, "destroy id=%d w=%p vtbl=%p\n", w->id,(void*)w,(void*)w->vtbl);
 *     (render)  fprintf(stderr, "render  id=%d w=%p vtbl=%p\n", w->id,(void*)w,(void*)w->vtbl);
 *   → 같은 주소가 destroy 후 render 에서 다시 나오고, vtbl 값이 달라져 있으면 UAF.
 *   (stdout 은 버퍼링되니 stderr 로 찍어야 크래시 직전 로그가 남는다)
 *
 * TODO: "해제"와 "슬롯 정리"를 한 곳에서 같이 하세요. 위젯 자신은 Screen 을 모르므로
 *       (dialog_on_event 는 self 만 안다) 이벤트 핸들러에서는 closed 표시만 남기고,
 *       Screen 쪽에서 closed 위젯을 free 한 뒤 그 슬롯을 NULL 로 만드는 편이 자연스럽습니다.
 *       이후 dispatch/render 루프가 NULL 슬롯을 건너뛰게 하세요. "해제 = 소유 포인터 무효화".
 */
#include <stdio.h>      // - printf, perror, snprintf 같은 입출력 함수 사용
#include <stdlib.h>     // - malloc, free, exit 사용
#include <string.h>     // - strncpy, memset 같은 문자열 관련 함수 사용

typedef struct Widget Widget;    // - Widget 구조체를 아래에서 정의하기 전에 이름만 먼저 만들어둠

typedef struct {
    void (*render)(Widget *self);                  // - 위젯을 화면에 그리는 함수의 주소를 저장
    void (*on_event)(Widget *self, int code);      // - 위젯이 이벤트를 처리하는 함수의 주소를 저장
} VTable;                                           // - 위젯이 사용할 함수들의 주소를 모아둔 구조체

struct Widget {
    const VTable *vtbl;     // - 어떤 종류의 위젯인지에 맞는 함수 주소들을 가리킴
    int id;                 // - 위젯을 구분하기 위한 번호
    int closed;             // - 위젯이 닫혔는지 나타냄
    char label[24];         // - 위젯에 표시할 문자열을 저장
};

#define MAX_WIDGETS 8       // - Screen 에 최대 8개의 위젯을 저장할 수 있게 정함
typedef struct {
    Widget *items[MAX_WIDGETS];     // - 위젯의 주소들을 배열로 저장
    int count;                       // - 현재 저장된 위젯의 개수
} Screen;                            // - 여러 위젯을 관리하는 구조체

/* ── 위젯 종류별 동작 ─────────────────────────────────────────── */
static void button_render(Widget *self) {     // - 버튼을 화면에 그리는 함수
    printf("  [Button #%d] \"%s\"\n", self->id, self->label);     // - 버튼의 번호와 이름을 출력
}
static void label_render(Widget *self) {      // - 라벨을 화면에 그리는 함수
    printf("  Label #%d: %s\n", self->id, self->label);          // - 라벨의 번호와 내용을 출력
}
static void dialog_render(Widget *self) {     // - 다이얼로그를 화면에 그리는 함수
    printf("  <<Dialog #%d>> %s\n", self->id, self->label);      // - 다이얼로그의 번호와 내용을 출력
}

static void widget_noop_event(Widget *self, int code) { (void)self; (void)code; }    // - 이벤트가 들어와도 아무 동작을 하지 않는 함수

/* 다이얼로그는 이벤트 코드 1(닫기)을 받으면 스스로 정리(파괴)된다 */
static void dialog_on_event(Widget *self, int code);     // - 다이얼로그의 이벤트 처리 함수가 있다고 미리 알려둠

static const VTable BUTTON_VT = { button_render, widget_noop_event };     // - 버튼이 사용할 함수 주소를 저장
static const VTable LABEL_VT  = { label_render,  widget_noop_event };     // - 라벨이 사용할 함수 주소를 저장
static const VTable DIALOG_VT = { dialog_render, dialog_on_event  };      // - 다이얼로그가 사용할 함수 주소를 저장

static Widget *widget_new(const VTable *vt, int id, const char *label) {     // - 새로운 위젯을 만들어 주소를 반환

    /* [Thinking Point]
    *   w 에 아직 아무 값도 넣지 않았는데, sizeof *w 로 *w 를 써도 괜찮은 이유는?
    *   tip 1. sizeof 는 피연산자를 '실행(역참조)'하지 않고 '타입'만 본다.
    *          → *w 의 타입(Widget)만 필요할 뿐, w 를 실제로 따라가지 않는다.
    *   tip 2. 그래서 sizeof *w 는 (VLA 제외) 컴파일 타임에 sizeof(Widget) 상수로 치환된다.
    *   생각해보기: sizeof(Widget) 대신 sizeof *w 로 쓰면 어떤 장점이 있을까?
    */
    Widget *w = malloc(sizeof *w);     // - Widget 하나를 저장할 메모리를 힙에서 할당하고 그 주소를 w에 저장
    if (!w) { perror("malloc"); exit(1); }     // - 메모리 할당에 실패하면 오류를 출력하고 프로그램 종료
    w->vtbl = vt;     // - 전달받은 함수 테이블의 주소를 위젯에 저장
    w->id = id;       // - 전달받은 번호를 위젯의 id에 저장
    w->closed = 0;    // - 처음에는 닫히지 않은 상태로 설정
    strncpy(w->label, label, sizeof(w->label) - 1);     // - 전달받은 문자열을 label 배열에 복사
    w->label[sizeof(w->label) - 1] = '\0';              // - 문자열의 마지막에 NULL 문자를 넣어 문자열 끝을 표시
    return w;     // - 만들어진 위젯의 주소를 호출한 곳으로 반환
}

static void widget_destroy(Widget *w) {     // - 위젯이 사용하던 힙 메모리를 해제하는 함수
    free(w);          // - w가 가리키는 메모리를 해제
}

/* ── Screen ──────────────────────────────────────────────────── */
static void screen_add(Screen *s, Widget *w) {     // - Screen 에 새로운 위젯을 추가하는 함수
    if (s->count < MAX_WIDGETS) s->items[s->count++] = w;     // - 빈 공간이 있으면 위젯 주소를 저장하고 count를 1 증가
}

static void screen_dispatch(Screen *s, int code) {     // - Screen 안의 모든 위젯에게 이벤트를 전달
    for (int i = 0; i < s->count; i++) {               // - 저장된 위젯 개수만큼 반복
        Widget *w = s->items[i];                       // - 현재 배열 위치의 위젯 주소를 w에 저장
        if (w == NULL)                                 // - 현재 슬롯에 위젯이 없으면
            continue;                                 // - 다음 위젯으로 넘어감
        w->vtbl->on_event(w, code);                    // - 현재 위젯의 이벤트 처리 함수를 호출
    }
}

static void screen_render(Screen *s) {                // - Screen 안의 모든 위젯을 화면에 그리는 함수
    for (int i = 0; i < s->count; i++) {               // - 저장된 위젯 개수만큼 반복
        Widget *w = s->items[i];                       // - 현재 배열 위치의 위젯 주소를 w에 저장
        if (w == NULL)                                 // - 현재 슬롯에 위젯이 없으면
            continue;                                 // - 다음 위젯으로 넘어감
        w->vtbl->render(w);                            // - 현재 위젯의 render 함수를 호출해서 화면에 그림
    }
}

static void dialog_on_event(Widget *self, int code) {     // - 다이얼로그에 들어온 이벤트를 처리하는 함수
    if (code == 1) {                                      // - 이벤트 코드가 1이면 닫기 이벤트로 처리
        self->closed = 1;                                 // - 실제 free는 하지 않고 닫힌 상태라는 표시만 남김
    }
}

static char *app_build_status(const char *text) {     // - 상태 메시지를 저장할 메모리를 만들고 주소를 반환
    char *msg = malloc(sizeof(Widget));               // - Widget 크기만큼 힙 메모리를 할당
    if (!msg) exit(1);                                 // - 메모리 할당에 실패하면 프로그램 종료

    //  [테스트용 연출] 재사용한 메모리를 0xAB 로 '일부러' 덮어써서 오염시킨다.
    //   실무라면 다른 기능이 우연히 이 자리를 덮어쓰겠지만, 여기서는 UAF 크래시를
    //   매번 똑같이(결정적으로) 재현하기 위해 인위적으로 채운다. 
    //  glibc(리눅스) 환경 (tcache)에서만 유효하다. 환경&상황에 따라 msg는 새로운 주소로 할당될 수 있다.
    
    memset(msg, 0xAB, sizeof(Widget));                 // - 할당받은 메모리 전체를 0xAB 값으로 채움
    snprintf(msg, sizeof(Widget), "STATUS: %s", text);  // - msg가 가리키는 메모리에 상태 메시지를 문자열로 저장
    return msg;                                         // - 상태 메시지가 저장된 메모리 주소를 반환
}

int main(void) {                                        // - 프로그램이 시작되는 함수
    Screen s = { .count = 0 };                           // - Screen을 만들고 현재 위젯 개수를 0으로 설정

    screen_add(&s, widget_new(&LABEL_VT,  10, "Welcome"));          // - 라벨 위젯을 만들고 Screen에 추가
    screen_add(&s, widget_new(&BUTTON_VT, 11, "OK"));               // - OK 버튼 위젯을 만들고 Screen에 추가
    screen_add(&s, widget_new(&DIALOG_VT, 12, "Are you sure?"));   // - 다이얼로그 위젯을 만들고 Screen에 추가
    screen_add(&s, widget_new(&BUTTON_VT, 13, "Cancel"));          // - Cancel 버튼 위젯을 만들고 Screen에 추가

    printf("frame 1:\n");                                  // - 첫 번째 화면이라는 문구를 출력
    screen_render(&s);                                    // - 현재 Screen에 있는 모든 위젯을 화면에 그림
    screen_dispatch(&s, 1);                               // - 모든 위젯에게 닫기 이벤트 코드 1을 전달

    /* TODO 닫힌(closed) 위젯을 여기서 정리(free + 해당 슬롯 NULL)할 필요가 있음 */
    for (int i = 0; i < s.count; i++) {                   // - Screen에 들어있는 위젯을 처음부터 끝까지 확인
        if (s.items[i] != NULL && s.items[i]->closed) {   // - 위젯이 존재하고 닫힌 상태인지 확인
            free(s.items[i]);                              // - 닫힌 위젯이 사용하던 힙 메모리를 해제
            s.items[i] = NULL;                             // - 해제한 위젯의 포인터를 NULL로 바꿔서 더 이상 사용하지 않게 함
        }
    }

    char *status = app_build_status("dialog closed");     // - 다이얼로그가 닫혔다는 상태 메시지를 만들고 주소를 저장
    printf("%s\n", status);                               // - 상태 메시지를 화면에 출력

    printf("frame 2:\n");                                 // - 두 번째 화면이라는 문구를 출력
    screen_render(&s);                                    // - NULL인 슬롯은 건너뛰고 남아있는 위젯만 그림

    free(status);                                         // - 상태 메시지에 사용한 힙 메모리를 해제
    for (int i = 0; i < s.count; i++) free(s.items[i]);   // - Screen에 남아있는 위젯들의 힙 메모리를 해제
    return 0;                                             // - 프로그램을 정상적으로 종료
}