#ifndef TERMEMU_IPC_PROTO_H
#define TERMEMU_IPC_PROTO_H

/*
 * IPC 프로토콜 — 데몬↔클라이언트 바이너리 메시지 포맷
 *
 * 전송 구조:
 *   [ ipc_msg_header_t (8 bytes) ][ payload (header.payload_len bytes) ]
 *
 * 바이트 순서: 리틀 엔디언 (x86/ARM 기본값).
 * 정렬: 모든 구조체는 __attribute__((packed)) 없이 자연 정렬을 따른다.
 *       플랫폼 간 호환이 필요한 경우 추후 직렬화 레이어를 추가한다.
 *
 * 메시지 방향:
 *   C→D  클라이언트 → 데몬
 *   D→C  데몬 → 클라이언트
 *   C↔D  양방향
 */

#include <stdint.h>
#include <stddef.h>

/* ─── 매직 / 버전 ────────────────────────────────────────────────────────── */

#define IPC_MAGIC   0x544D55  /* "TMU" (3 bytes, little-endian 하위 24 bit) */
#define IPC_VERSION 1

/* ─── 메시지 타입 ────────────────────────────────────────────────────────── */

typedef enum {
    /* ── 연결 관리 ─────────────────────── */
    IPC_MSG_HELLO           = 0x01,  /* C→D  핸드셰이크 요청 */
    IPC_MSG_HELLO_ACK       = 0x02,  /* D→C  핸드셰이크 응답 */
    IPC_MSG_BYE             = 0x03,  /* C↔D  연결 종료 통보 */

    /* ── 세션 ───────────────────────────── */
    IPC_MSG_SESSION_CREATE  = 0x10,  /* C→D  세션 생성 요청 */
    IPC_MSG_SESSION_CREATED = 0x11,  /* D→C  세션 생성 완료 (id 포함) */
    IPC_MSG_SESSION_DESTROY = 0x12,  /* C→D  세션 삭제 요청 */
    IPC_MSG_SESSION_LIST    = 0x13,  /* C→D  세션 목록 요청 */
    IPC_MSG_SESSION_LIST_R  = 0x14,  /* D→C  세션 목록 응답 */
    IPC_MSG_SESSION_ATTACH  = 0x15,  /* C→D  기존 세션 attach 요청 */
    IPC_MSG_SESSION_ATTACH_R = 0x16, /* D→C  세션 트리 (페인 목록) 응답 */
    IPC_MSG_PANE_REPLAY     = 0x17,  /* C→D  pane PTY 출력 히스토리 요청 */
    IPC_MSG_SESSION_SAVE    = 0x18,  /* C→D  세션 스냅샷 요청 (JSON 응답) */
    IPC_MSG_SESSION_SAVE_R  = 0x19,  /* D→C  세션 스냅샷 JSON 페이로드 */

    /* ── 윈도우 ─────────────────────────── */
    IPC_MSG_WINDOW_CREATE   = 0x20,  /* C→D  윈도우 생성 요청 */
    IPC_MSG_WINDOW_CREATED  = 0x21,  /* D→C  윈도우 생성 완료 */
    IPC_MSG_WINDOW_DESTROY  = 0x22,  /* C→D  윈도우 삭제 요청 */
    IPC_MSG_WINDOW_FOCUS    = 0x23,  /* C→D  active window 변경 */
    IPC_MSG_WINDOW_LAYOUT   = 0x24,  /* C→D  레이아웃 트리 blob 업데이트 (재접속 복원용) */

    /* ── 페인 ───────────────────────────── */
    IPC_MSG_PANE_CREATE     = 0x30,  /* C→D  페인 생성 + PTY 스폰 요청 */
    IPC_MSG_PANE_CREATED    = 0x31,  /* D→C  페인 생성 완료 (pane_id 포함) */
    IPC_MSG_PANE_DESTROY    = 0x32,  /* C→D  페인 삭제 요청 */
    IPC_MSG_PANE_RESIZE     = 0x33,  /* C→D  터미널 크기 변경 */
    IPC_MSG_PANE_FOCUS      = 0x34,  /* C→D  active pane 변경 */
    IPC_MSG_PANE_EXITED     = 0x35,  /* D→C  PTY 종료 (셸 exit) 통보 */
    IPC_MSG_PANE_SPLIT_NOTIFY = 0x36, /* D→C  다른 클라이언트의 split 통지 (브로드캐스트) */

    /* ── 데이터 ─────────────────────────── */
    IPC_MSG_PTY_INPUT       = 0x40,  /* C→D  키보드/마우스 → PTY stdin */
    IPC_MSG_PTY_OUTPUT      = 0x41,  /* D→C  PTY stdout/stderr → 클라이언트 */

    /* ── 화면 상태 ──────────────────────── */
    IPC_MSG_SCREEN_DAMAGE   = 0x50,  /* D→C  변경된 셀 범위 통보 */
    IPC_MSG_SCREEN_SNAPSHOT = 0x51,  /* C→D  전체 화면 스냅샷 요청 */
    IPC_MSG_SCREEN_SNAPSHOT_R = 0x52, /* D→C  전체 화면 스냅샷 응답 */

    /* ── 공통 응답 ──────────────────────── */
    IPC_MSG_OK              = 0xF0,  /* D→C  요청 성공 (payload 없음) */
    IPC_MSG_ERROR           = 0xF1,  /* D→C  요청 실패 (ipc_payload_error_t) */
} ipc_msg_type_t;

/* ─── 공통 헤더 (8 bytes) ────────────────────────────────────────────────── */

typedef struct {
    uint32_t magic_ver;   /* 상위 8 bit: version, 하위 24 bit: IPC_MAGIC */
    uint8_t  type;        /* ipc_msg_type_t */
    uint8_t  flags;       /* 예약 (현재 0) */
    uint16_t payload_len; /* 뒤따르는 페이로드 바이트 수 (0 가능) */
} ipc_msg_header_t;

/* 헤더 초기화 헬퍼 */
#define IPC_HEADER_INIT(msg_type, plen) \
    { ((IPC_VERSION) << 24) | (IPC_MAGIC), (msg_type), 0, (plen) }

/* ─── 페이로드 구조체 ────────────────────────────────────────────────────── */

/* IPC_MSG_HELLO  C→D */
typedef struct {
    uint32_t client_pid;
    char     client_version[16]; /* "0.1.0\0..." */
} ipc_payload_hello_t;

/* IPC_MSG_HELLO_ACK  D→C */
typedef struct {
    uint32_t daemon_pid;
    char     daemon_version[16];
    uint32_t session_count;  /* 현재 활성 세션 수 */
} ipc_payload_hello_ack_t;

/* IPC_MSG_SESSION_CREATE  C→D */
typedef struct {
    char name[64];  /* SESSION_NAME_MAX */
} ipc_payload_session_create_t;

/* IPC_MSG_SESSION_CREATED  D→C */
typedef struct {
    uint32_t session_id;
    char     name[64];
} ipc_payload_session_created_t;

/* IPC_MSG_SESSION_DESTROY  C→D */
typedef struct {
    uint32_t session_id;
} ipc_payload_session_destroy_t;

/* IPC_MSG_SESSION_LIST_R  D→C
 * 페이로드: ipc_session_info_t 배열 (header.payload_len / sizeof(ipc_session_info_t) 개) */
typedef struct {
    uint32_t session_id;
    uint32_t window_count;
    char     name[64];
} ipc_session_info_t;

/* IPC_MSG_WINDOW_CREATE  C→D */
typedef struct {
    uint32_t session_id;
    char     name[64];  /* WINDOW_NAME_MAX */
} ipc_payload_window_create_t;

/* IPC_MSG_WINDOW_CREATED  D→C */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
    char     name[64];
} ipc_payload_window_created_t;

/* IPC_MSG_WINDOW_DESTROY / IPC_MSG_WINDOW_FOCUS  C→D */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
} ipc_payload_window_ref_t;

/* IPC_MSG_WINDOW_LAYOUT  C→D
 * 페이로드: 이 헤더 + blob[blob_len]. daemon 은 layout 포맷을 해석하지 않고
 * 원본을 저장했다가 SESSION_ATTACH_R 응답에 동봉한다. */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
    uint16_t blob_len;
    uint8_t  reserved[2];
    /* uint8_t blob[blob_len] — 뒤에 인라인 */
} ipc_payload_window_layout_t;

/* IPC_MSG_PANE_CREATE  C→D
 * parent_pane_id != 0 이면 split 컨텍스트 — 데몬은 PANE_SPLIT_NOTIFY 를
 * 다른 클라이언트들에 브로드캐스트한다. parent_pane_id == 0 은 첫 pane 생성. */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
    uint16_t cols;
    uint16_t rows;
    uint32_t parent_pane_id;
    uint8_t  direction;     /* 0 = LAYOUT_SPLIT_H, 1 = LAYOUT_SPLIT_V */
    uint8_t  reserved[3];
    float    ratio;
} ipc_payload_pane_create_t;

/* IPC_MSG_PANE_SPLIT_NOTIFY  D→C */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
    uint32_t parent_pane_id;
    uint32_t new_pane_id;
    uint16_t cols;
    uint16_t rows;
    uint8_t  direction;
    uint8_t  reserved[3];
    float    ratio;
} ipc_payload_pane_split_notify_t;

/* IPC_MSG_PANE_CREATED  D→C */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
    uint32_t pane_id;
    int32_t  pid;    /* 스폰된 자식 프로세스 PID */
} ipc_payload_pane_created_t;

/* IPC_MSG_PANE_DESTROY / IPC_MSG_PANE_FOCUS  C→D */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
    uint32_t pane_id;
} ipc_payload_pane_ref_t;

/* IPC_MSG_PANE_RESIZE  C→D */
typedef struct {
    uint32_t session_id;
    uint32_t window_id;
    uint32_t pane_id;
    uint16_t cols;
    uint16_t rows;
} ipc_payload_pane_resize_t;

/* IPC_MSG_PTY_INPUT  C→D
 * 페이로드: ipc_payload_pty_data_t 헤더 + data[data_len] */
typedef struct {
    uint32_t pane_id;
    uint16_t data_len;
    /* uint8_t data[]; — 가변 길이, 구조체 뒤에 인라인 */
} ipc_payload_pty_data_t;

/* IPC_MSG_PTY_OUTPUT  D→C — 동일 구조 재사용 */

/* IPC_MSG_SESSION_ATTACH  C→D */
typedef struct {
    uint32_t session_id;
} ipc_payload_session_attach_t;

/* IPC_MSG_SESSION_ATTACH_R  D→C
 * 페이로드: 이 헤더 + ipc_attach_pane_info_t[pane_count] + layout_blob[layout_blob_len].
 * layout_blob 은 window_id=first_window 의 직렬화된 layout tree. */
typedef struct {
    uint32_t session_id;
    uint32_t pane_count;
    char     session_name[64];
    uint16_t layout_blob_len;  /* 0 = layout 정보 없음 (이전 버전 호환 시 flat split 사용) */
    uint8_t  reserved[2];
} ipc_payload_session_attach_r_t;

typedef struct {
    uint32_t pane_id;
    uint32_t window_id;
    uint16_t cols;
    uint16_t rows;
} ipc_attach_pane_info_t;

/* IPC_MSG_SCREEN_DAMAGE  D→C */
typedef struct {
    uint32_t pane_id;
    uint16_t x;      /* 손상 영역 좌상단 열 (0-based) */
    uint16_t y;      /* 손상 영역 좌상단 행 (0-based) */
    uint16_t width;  /* 손상 너비 (열 수) */
    uint16_t height; /* 손상 높이 (행 수) */
} ipc_payload_screen_damage_t;

/* IPC_MSG_SCREEN_SNAPSHOT  C→D */
typedef struct {
    uint32_t pane_id;
} ipc_payload_screen_snapshot_t;

/* IPC_MSG_ERROR  D→C */
typedef enum {
    IPC_ERR_UNKNOWN          = 0,
    IPC_ERR_INVALID_MSG      = 1,
    IPC_ERR_SESSION_NOT_FOUND = 2,
    IPC_ERR_WINDOW_NOT_FOUND = 3,
    IPC_ERR_PANE_NOT_FOUND   = 4,
    IPC_ERR_PTY_SPAWN_FAILED = 5,
    IPC_ERR_NAME_CONFLICT    = 6,
    IPC_ERR_LIMIT_REACHED    = 7,
} ipc_error_code_t;

typedef struct {
    uint32_t request_type;   /* 실패한 요청의 ipc_msg_type_t */
    uint32_t error_code;     /* ipc_error_code_t */
    char     message[128];   /* 사람이 읽을 수 있는 오류 설명 */
} ipc_payload_error_t;

/* ─── 소켓 경로 ──────────────────────────────────────────────────────────── */

/* POSIX: /tmp/termemu-<uid>.sock  (런타임에 uid 치환) */
#define IPC_SOCKET_PATH_FMT  "/tmp/termemu-%u.sock"
#define IPC_SOCKET_PATH_MAX  64

/* Windows: \\.\pipe\termemu-<username> */
#define IPC_PIPE_NAME_FMT    "\\\\.\\pipe\\termemu-%s"
#define IPC_PIPE_NAME_MAX    128

/* ─── 크기 제약 ──────────────────────────────────────────────────────────── */

/* 단일 메시지의 최대 페이로드 크기 (64 KiB — uint16_t payload_len 최대값) */
#define IPC_MAX_PAYLOAD_LEN  UINT16_MAX

/* PTY 데이터 단위 최대 크기 (read() 호출 버퍼) */
#define IPC_PTY_CHUNK_MAX    4096

#endif /* TERMEMU_IPC_PROTO_H */
