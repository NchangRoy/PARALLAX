/* ============================================================================
 * PARALLAX - Network Thread (version monolithique)
 * parallax_netthread.c - Tout le code de production en un seul fichier
 * ============================================================================
 *
 * Ce fichier regroupe tout ce qui formait auparavant :
 *   net_protocol.{h,c}  + net_config.{h,c}  +
 *   net_retry.{h,c}     + net_core.{h,c}    +  main.c
 *
 * Compilation :
 *   gcc -std=gnu11 -Wall -Wextra -O2 -g parallax_netthread.c \
 *       -o parallax_netthread -lrt -lpthread
 *
 * Usage :
 *   ./parallax_netthread <chemin_config.ini>
 *
 * Le binaire est rigoureusement le meme sur master, controleur et worker.
 * Tout ce qui change est dans le fichier de config.
 *
 * Plan du fichier (chercher avec /=== pour naviguer) :
 *
 *   ===== PARTIE 1 - DECLARATIONS =====
 *     1.1  Includes et constantes
 *     1.2  Types : protocole (header, paquets)
 *     1.3  Types : config + table des pairs
 *     1.4  Types : retry queue
 *     1.5  Types : etat global du Network Thread
 *     1.6  Prototypes des fonctions publiques et internes
 *
 *   ===== PARTIE 2 - PROTOCOLE =====
 *     2.1  Compteur de pkt_id et timestamp
 *     2.2  CRC32 IEEE 802.3
 *     2.3  Helpers little-endian
 *     2.4  Pack / unpack du header
 *     2.5  Verification d'integrite, libration
 *
 *   ===== PARTIE 3 - CONFIG ET PAIRS =====
 *     3.1  Helpers UUID
 *     3.2  Mapping nom <-> service_id
 *     3.3  Defaults et chargement INI
 *     3.4  Peer table (init/upsert/lookup/touch/remove/foreach)
 *
 *   ===== PARTIE 4 - RETRY QUEUE =====
 *     4.1  I/O fiables
 *     4.2  Backoff exponentiel
 *     4.3  Replay au demarrage
 *     4.4  Append disque
 *     4.5  API publique (init/push/pick/mark/compact/destroy)
 *
 *   ===== PARTIE 5 - NETWORK CORE =====
 *     5.1  Helpers internes (log, fcntl, conn slots)
 *     5.2  Initialisation (sockets, queues, epoll)
 *     5.3  HELLO (decouverte des pairs)
 *     5.4  Envoi (TCP/UDP + retry sur echec)
 *     5.5  Dispatcher (paquet -> queue _in du service local)
 *     5.6  Reception UDP et TCP
 *     5.7  Thread sondeur des queues _out
 *     5.8  Cycle de vie (init / run / stop / shutdown)
 *
 *   ===== PARTIE 6 - MAIN =====
 *
 * ============================================================================ */

/* ============================================================================
 * ===== PARTIE 1 - DECLARATIONS =====
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * 1.1  Includes et constantes
 * -------------------------------------------------------------------------- */

#define _GNU_SOURCE

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- Constantes du protocole ---- */

#define PARALLAX_MAGIC          0x504C5841u  /* "PLXA" en ASCII */
#define PARALLAX_PROTO_VERSION  1
#define PARALLAX_UUID_LEN       16
#define PARALLAX_MAX_PAYLOAD    (4 * 1024 * 1024)  /* 4 MB par paquet */
#define PARALLAX_HEADER_SIZE    64

/* ---- Limites de l'implementation ---- */

#define PARALLAX_MAX_SERVICES   10
#define PARALLAX_MAX_PEERS      256
#define PARALLAX_QNAME_LEN      64
#define PARALLAX_PATH_LEN       256
#define RETRY_MAX_ENTRIES       1024
#define NET_MAX_TCP_CONNS       64
#define NET_MAX_EPOLL_EVT       64

/* ---- Types de paquets ---- */

typedef enum {
    PKT_HELLO        = 1,
    PKT_HELLO_ACK    = 2,
    PKT_HEARTBEAT    = 3,
    PKT_GOSSIP       = 4,
    PKT_DATA         = 10,
    PKT_TASK_ASSIGN  = 11,
    PKT_TASK_RESULT  = 12,
    PKT_STATE_REPORT = 13,
    PKT_QUERY        = 14,
    PKT_QUERY_REPLY  = 15,
    PKT_ELECTION     = 20,
    PKT_COORD        = 21,
    PKT_ALIVE        = 22,
    PKT_ACK          = 30,
    PKT_PING         = 40,
    PKT_PONG         = 41
} pkt_type_t;

/* ---- Identifiants de service (routage interne) ---- */

typedef enum {
    SVC_NONE             = 0,
    SVC_EXECUTION        = 1,
    SVC_MONITORING       = 2,
    SVC_STATE_RECEIVER   = 3,
    SVC_MASTER_SERVANT   = 4,
    SVC_PANNE_DETECTOR   = 5,
    SVC_EXECUTION_MASTER = 6,
    SVC_PARSER           = 7,
    SVC_ORCHESTRATOR     = 8,
    SVC_NETWORK          = 99
} service_id_t;

/* ---- Drapeaux du header ---- */

#define PKT_FLAG_NONE         0x00
#define PKT_FLAG_ACK_REQUIRED 0x01
#define PKT_FLAG_BROADCAST    0x02
#define PKT_FLAG_RETRY        0x04
#define PKT_FLAG_FRAGMENTED   0x08

/* ----------------------------------------------------------------------------
 * 1.2  Types : protocole
 * -------------------------------------------------------------------------- */

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  pkt_type;
    uint8_t  service_id;
    uint8_t  flags;
    uint8_t  src_uuid[PARALLAX_UUID_LEN];
    uint8_t  dst_uuid[PARALLAX_UUID_LEN];
    uint64_t pkt_id;
    uint64_t timestamp_ms;
    uint32_t payload_len;
    uint32_t crc32;
} net_header_t;

_Static_assert(
    sizeof(uint32_t) + 4 + PARALLAX_UUID_LEN * 2 + 8 + 8 + 4 + 4 == PARALLAX_HEADER_SIZE,
    "PARALLAX_HEADER_SIZE doit valoir 64"
);

typedef struct {
    net_header_t header;
    uint8_t     *payload;
} net_packet_t;

/* ----------------------------------------------------------------------------
 * 1.3  Types : config + table des pairs
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  uuid[PARALLAX_UUID_LEN];
    char     role[16];

    uint16_t tcp_port;
    uint16_t udp_port;
    char     broadcast_addr[INET_ADDRSTRLEN];

    int          num_services;
    service_id_t services[PARALLAX_MAX_SERVICES];

    char     retry_path[PARALLAX_PATH_LEN];
    int      retry_max_attempts;
    int      retry_base_backoff_ms;

    int      heartbeat_period_ms;
    int      gossip_period_ms;
    int      suspect_timeout_ms;
    int      failed_timeout_ms;
} net_config_t;

typedef struct {
    uint8_t  uuid[PARALLAX_UUID_LEN];
    char     ip[INET_ADDRSTRLEN];
    uint16_t tcp_port;
    uint16_t udp_port;
    uint64_t last_seen_ms;
    int      in_use;
} peer_entry_t;

typedef struct {
    peer_entry_t     entries[PARALLAX_MAX_PEERS];
    int              count;
    pthread_rwlock_t lock;
} peer_table_t;

typedef int (*peer_iter_fn)(const peer_entry_t *entry, void *user);

/* ----------------------------------------------------------------------------
 * 1.4  Types : retry queue
 * -------------------------------------------------------------------------- */

typedef struct {
    uint32_t  attempts;
    uint64_t  next_try_ms;
    uint8_t  *frame;
    uint32_t  frame_len;
    int       in_use;
    int       delivered;
} retry_entry_t;

typedef struct {
    retry_entry_t   entries[RETRY_MAX_ENTRIES];
    int             count;
    int             fd;
    char            path[256];
    int             max_attempts;
    int             base_backoff_ms;
    pthread_mutex_t lock;
} retry_queue_t;

/* ----------------------------------------------------------------------------
 * 1.5  Types : etat global du Network Thread
 * -------------------------------------------------------------------------- */

typedef struct {
    int       fd;
    int       in_use;
    char      peer_ip[INET_ADDRSTRLEN];
    uint16_t  peer_port;
    uint8_t  *rx_buf;
    size_t    rx_buf_cap;
    size_t    rx_buf_len;
} tcp_conn_t;

typedef struct {
    service_id_t id;
    mqd_t        mq_in;
    mqd_t        mq_out;
    char         name_in [PARALLAX_QNAME_LEN];
    char         name_out[PARALLAX_QNAME_LEN];
} service_qpair_t;

typedef struct {
    net_config_t    cfg;

    int             tcp_listen_fd;
    int             udp_fd;
    int             epoll_fd;

    tcp_conn_t      tcp_conns[NET_MAX_TCP_CONNS];

    service_qpair_t qpairs[PARALLAX_MAX_SERVICES];
    int             num_qpairs;

    peer_table_t    peers;
    retry_queue_t   retry;

    volatile int    running;
    pthread_t       outq_thread;
} net_state_t;

/* ----------------------------------------------------------------------------
 * 1.6  Prototypes (forward declarations)
 * -------------------------------------------------------------------------- */

/* Protocole */
static int      net_protocol_pack_header(const net_header_t *hdr, uint8_t *buf);
static int      net_protocol_unpack_header(const uint8_t *buf, net_header_t *hdr);
static uint32_t net_protocol_crc32(const uint8_t *data, size_t len);
static int      net_protocol_verify(const net_packet_t *pkt);
static void     net_protocol_free_packet(net_packet_t *pkt);
static uint64_t net_protocol_next_pkt_id(void);
static uint64_t net_protocol_now_ms(void);

/* Config + UUIDs + queues */
static int          net_config_load(const char *path, net_config_t *cfg);
static void         net_config_set_defaults(net_config_t *cfg);
static int          net_config_queue_name(service_id_t svc, int is_input,
                                          char *out_buf, size_t out_len);
static service_id_t net_config_service_from_name(const char *name);
static const char  *net_config_service_to_name(service_id_t svc);

static int  uuid_equal(const uint8_t *a, const uint8_t *b);
static int  uuid_is_zero(const uint8_t *u);
static void uuid_to_hex(const uint8_t *uuid, char *out);
static int  uuid_from_hex(const char *hex, uint8_t *out);

/* Peer table */
static int  peer_table_init(peer_table_t *tbl);
static void peer_table_destroy(peer_table_t *tbl);
static int  peer_table_upsert(peer_table_t *tbl, const uint8_t *uuid,
                              const char *ip, uint16_t tcp_port, uint16_t udp_port);
static int  peer_table_lookup(peer_table_t *tbl, const uint8_t *uuid,
                              peer_entry_t *out);
static int  peer_table_touch(peer_table_t *tbl, const uint8_t *uuid);

/* Retry queue */
static int  retry_queue_init(retry_queue_t *q, const char *path,
                             int max_attempts, int base_backoff_ms);
static int  retry_queue_push(retry_queue_t *q, uint8_t *frame, uint32_t frame_len);
static int  retry_queue_pick(retry_queue_t *q, uint8_t **out_frame, uint32_t *out_len);
static void retry_queue_mark_attempt(retry_queue_t *q, int idx, int success);
static int  retry_queue_compact(retry_queue_t *q);
static void retry_queue_destroy(retry_queue_t *q);

/* Network core */
static int  net_core_init(net_state_t *st, const char *config_path);
static int  net_core_run(net_state_t *st);
static void net_core_stop(net_state_t *st);
static void net_core_shutdown(net_state_t *st);
static int  net_core_send_packet(net_state_t *st, net_packet_t *pkt);
static int  net_core_broadcast_hello(net_state_t *st);

/* ============================================================================
 * ===== PARTIE 2 - PROTOCOLE =====
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * 2.1  Compteur de pkt_id et timestamp
 * -------------------------------------------------------------------------- */

static pthread_mutex_t g_id_mtx = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_id_counter = 0;

static uint64_t net_protocol_next_pkt_id(void)
{
    uint32_t low;
    pthread_mutex_lock(&g_id_mtx);
    low = ++g_id_counter;
    pthread_mutex_unlock(&g_id_mtx);

    uint64_t high = (uint64_t)(time(NULL) & 0xFFFFFFFF);
    return (high << 32) | (uint64_t)low;
}

static uint64_t net_protocol_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ----------------------------------------------------------------------------
 * 2.2  CRC32 IEEE 802.3
 * -------------------------------------------------------------------------- */

static uint32_t      g_crc_table[256];
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;

static void crc_table_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
}

static uint32_t net_protocol_crc32(const uint8_t *data, size_t len)
{
    pthread_once(&g_crc_once, crc_table_init);

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = g_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ----------------------------------------------------------------------------
 * 2.3  Helpers little-endian
 * -------------------------------------------------------------------------- */

static void put_u32(uint8_t *buf, size_t offset, uint32_t v)
{
    uint32_t le = htole32(v);
    memcpy(buf + offset, &le, sizeof(le));
}

static void put_u64(uint8_t *buf, size_t offset, uint64_t v)
{
    uint64_t le = htole64(v);
    memcpy(buf + offset, &le, sizeof(le));
}

static uint32_t get_u32(const uint8_t *buf, size_t offset)
{
    uint32_t le;
    memcpy(&le, buf + offset, sizeof(le));
    return le32toh(le);
}

static uint64_t get_u64(const uint8_t *buf, size_t offset)
{
    uint64_t le;
    memcpy(&le, buf + offset, sizeof(le));
    return le64toh(le);
}

/* ----------------------------------------------------------------------------
 * 2.4  Pack / unpack du header
 *
 * Layout sur le wire (offsets en octets) :
 *   0  : magic         (u32)
 *   4  : version       (u8)
 *   5  : pkt_type      (u8)
 *   6  : service_id    (u8)
 *   7  : flags         (u8)
 *   8  : src_uuid      (16 octets)
 *   24 : dst_uuid      (16 octets)
 *   40 : pkt_id        (u64)
 *   48 : timestamp_ms  (u64)
 *   56 : payload_len   (u32)
 *   60 : crc32         (u32)
 *   total : 64 octets
 * -------------------------------------------------------------------------- */

static int net_protocol_pack_header(const net_header_t *hdr, uint8_t *buf)
{
    if (!hdr || !buf) return -1;
    memset(buf, 0, PARALLAX_HEADER_SIZE);

    put_u32(buf, 0, hdr->magic);
    buf[4] = hdr->version;
    buf[5] = hdr->pkt_type;
    buf[6] = hdr->service_id;
    buf[7] = hdr->flags;
    memcpy(buf + 8,  hdr->src_uuid, PARALLAX_UUID_LEN);
    memcpy(buf + 24, hdr->dst_uuid, PARALLAX_UUID_LEN);
    put_u64(buf, 40, hdr->pkt_id);
    put_u64(buf, 48, hdr->timestamp_ms);
    put_u32(buf, 56, hdr->payload_len);
    put_u32(buf, 60, hdr->crc32);
    return 0;
}

static int net_protocol_unpack_header(const uint8_t *buf, net_header_t *hdr)
{
    if (!buf || !hdr) return -1;

    hdr->magic       = get_u32(buf, 0);
    hdr->version     = buf[4];
    hdr->pkt_type    = buf[5];
    hdr->service_id  = buf[6];
    hdr->flags       = buf[7];
    memcpy(hdr->src_uuid, buf + 8,  PARALLAX_UUID_LEN);
    memcpy(hdr->dst_uuid, buf + 24, PARALLAX_UUID_LEN);
    hdr->pkt_id       = get_u64(buf, 40);
    hdr->timestamp_ms = get_u64(buf, 48);
    hdr->payload_len  = get_u32(buf, 56);
    hdr->crc32        = get_u32(buf, 60);

    if (hdr->magic   != PARALLAX_MAGIC)         return -1;
    if (hdr->version != PARALLAX_PROTO_VERSION) return -1;
    if (hdr->payload_len > PARALLAX_MAX_PAYLOAD) return -1;
    return 0;
}

/* ----------------------------------------------------------------------------
 * 2.5  Verification d'integrite, liberation
 * -------------------------------------------------------------------------- */

static int net_protocol_verify(const net_packet_t *pkt)
{
    if (!pkt) return -1;
    if (pkt->header.magic   != PARALLAX_MAGIC)         return -2;
    if (pkt->header.version != PARALLAX_PROTO_VERSION) return -3;
    if (pkt->header.payload_len > PARALLAX_MAX_PAYLOAD) return -4;
    if (pkt->header.payload_len > 0 && pkt->payload == NULL) return -5;

    if (pkt->header.payload_len > 0) {
        uint32_t computed = net_protocol_crc32(pkt->payload, pkt->header.payload_len);
        if (computed != pkt->header.crc32) return -6;
    }
    return 0;
}

static void net_protocol_free_packet(net_packet_t *pkt)
{
    if (!pkt) return;
    if (pkt->payload) {
        free(pkt->payload);
        pkt->payload = NULL;
    }
    pkt->header.payload_len = 0;
}

/* ============================================================================
 * ===== PARTIE 3 - CONFIG ET PAIRS =====
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * 3.1  Helpers UUID
 * -------------------------------------------------------------------------- */

static int uuid_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, PARALLAX_UUID_LEN) == 0;
}

static int uuid_is_zero(const uint8_t *u)
{
    for (int i = 0; i < PARALLAX_UUID_LEN; ++i) {
        if (u[i] != 0) return 0;
    }
    return 1;
}

static void uuid_to_hex(const uint8_t *uuid, char *out)
{
    static const char hex_digits[] = "0123456789abcdef";
    for (int i = 0; i < PARALLAX_UUID_LEN; ++i) {
        out[2 * i]     = hex_digits[(uuid[i] >> 4) & 0xF];
        out[2 * i + 1] = hex_digits[uuid[i] & 0xF];
    }
    out[2 * PARALLAX_UUID_LEN] = '\0';
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int uuid_from_hex(const char *hex, uint8_t *out)
{
    if (!hex || strlen(hex) < 2 * PARALLAX_UUID_LEN) return -1;
    for (int i = 0; i < PARALLAX_UUID_LEN; ++i) {
        int hi = hex_digit(hex[2 * i]);
        int lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ----------------------------------------------------------------------------
 * 3.2  Mapping nom <-> service_id
 * -------------------------------------------------------------------------- */

typedef struct { const char *name; service_id_t id; } svc_map_t;

static const svc_map_t g_svc_map[] = {
    { "execution",        SVC_EXECUTION        },
    { "monitoring",       SVC_MONITORING       },
    { "state_receiver",   SVC_STATE_RECEIVER   },
    { "master_servant",   SVC_MASTER_SERVANT   },
    { "panne_detector",   SVC_PANNE_DETECTOR   },
    { "execution_master", SVC_EXECUTION_MASTER },
    { "parser",           SVC_PARSER           },
    { "orchestrator",     SVC_ORCHESTRATOR     },
    { "network",          SVC_NETWORK          },
    { NULL,               SVC_NONE             }
};

static service_id_t net_config_service_from_name(const char *name)
{
    if (!name) return SVC_NONE;
    for (int i = 0; g_svc_map[i].name; ++i) {
        if (strcasecmp(g_svc_map[i].name, name) == 0) return g_svc_map[i].id;
    }
    return SVC_NONE;
}

static const char *net_config_service_to_name(service_id_t svc)
{
    for (int i = 0; g_svc_map[i].name; ++i) {
        if (g_svc_map[i].id == svc) return g_svc_map[i].name;
    }
    return "unknown";
}

static int net_config_queue_name(service_id_t svc, int is_input,
                                 char *out_buf, size_t out_len)
{
    const char *svc_name = net_config_service_to_name(svc);
    int n = snprintf(out_buf, out_len, "/parallax_%s_%s",
                     svc_name, is_input ? "in" : "out");
    return (n > 0 && (size_t)n < out_len) ? 0 : -1;
}

/* ----------------------------------------------------------------------------
 * 3.3  Defaults et chargement INI
 * -------------------------------------------------------------------------- */

static void net_config_set_defaults(net_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->role, "worker", sizeof(cfg->role) - 1);
    cfg->tcp_port = 7777;
    cfg->udp_port = 7778;
    strncpy(cfg->broadcast_addr, "255.255.255.255", sizeof(cfg->broadcast_addr) - 1);
    strncpy(cfg->retry_path, "/tmp/parallax_retry.log", sizeof(cfg->retry_path) - 1);
    cfg->retry_max_attempts    = 5;
    cfg->retry_base_backoff_ms = 500;
    cfg->heartbeat_period_ms   = 2000;
    cfg->gossip_period_ms      = 5000;
    cfg->suspect_timeout_ms    = 4000;
    cfg->failed_timeout_ms     = 8000;
}

static char *str_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return s;
}

static void parse_service_list(const char *list, net_config_t *cfg)
{
    char buf[256];
    strncpy(buf, list, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    cfg->num_services = 0;
    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token && cfg->num_services < PARALLAX_MAX_SERVICES) {
        token = str_trim(token);
        service_id_t id = net_config_service_from_name(token);
        if (id != SVC_NONE) cfg->services[cfg->num_services++] = id;
        token = strtok_r(NULL, ",", &saveptr);
    }
}

static int net_config_load(const char *path, net_config_t *cfg)
{
    if (!path || !cfg) return -1;
    net_config_set_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[config] cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[512];
    char section[64] = "";
    int  uuid_set = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = str_trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) continue;
            *end = '\0';
            strncpy(section, p + 1, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = str_trim(p);
        char *val = str_trim(eq + 1);

        if (strcmp(section, "node") == 0) {
            if (strcmp(key, "uuid") == 0) {
                if (uuid_from_hex(val, cfg->uuid) == 0) uuid_set = 1;
            } else if (strcmp(key, "role") == 0) {
                strncpy(cfg->role, val, sizeof(cfg->role) - 1);
            }
        } else if (strcmp(section, "network") == 0) {
            if (strcmp(key, "tcp_port") == 0) cfg->tcp_port = (uint16_t)atoi(val);
            else if (strcmp(key, "udp_port") == 0) cfg->udp_port = (uint16_t)atoi(val);
            else if (strcmp(key, "broadcast_addr") == 0)
                strncpy(cfg->broadcast_addr, val, sizeof(cfg->broadcast_addr) - 1);
        } else if (strcmp(section, "services") == 0) {
            if (strcmp(key, "list") == 0) parse_service_list(val, cfg);
        } else if (strcmp(section, "retry") == 0) {
            if (strcmp(key, "path") == 0)
                strncpy(cfg->retry_path, val, sizeof(cfg->retry_path) - 1);
            else if (strcmp(key, "max_attempts") == 0) cfg->retry_max_attempts = atoi(val);
            else if (strcmp(key, "base_backoff_ms") == 0) cfg->retry_base_backoff_ms = atoi(val);
        } else if (strcmp(section, "heartbeat") == 0) {
            if (strcmp(key, "period_ms") == 0) cfg->heartbeat_period_ms = atoi(val);
            else if (strcmp(key, "gossip_period_ms") == 0) cfg->gossip_period_ms = atoi(val);
            else if (strcmp(key, "suspect_timeout_ms") == 0) cfg->suspect_timeout_ms = atoi(val);
            else if (strcmp(key, "failed_timeout_ms") == 0) cfg->failed_timeout_ms = atoi(val);
        }
    }
    fclose(f);

    if (!uuid_set) {
        fprintf(stderr, "[config] missing or invalid uuid in %s\n", path);
        return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------------------
 * 3.4  Peer table
 * -------------------------------------------------------------------------- */

static int peer_table_init(peer_table_t *tbl)
{
    memset(tbl, 0, sizeof(*tbl));
    return pthread_rwlock_init(&tbl->lock, NULL);
}

static void peer_table_destroy(peer_table_t *tbl)
{
    pthread_rwlock_destroy(&tbl->lock);
}

static int peer_find_locked(peer_table_t *tbl, const uint8_t *uuid)
{
    for (int i = 0; i < PARALLAX_MAX_PEERS; ++i) {
        if (tbl->entries[i].in_use && uuid_equal(tbl->entries[i].uuid, uuid)) return i;
    }
    return -1;
}

static int peer_table_upsert(peer_table_t *tbl, const uint8_t *uuid,
                             const char *ip, uint16_t tcp_port, uint16_t udp_port)
{
    pthread_rwlock_wrlock(&tbl->lock);

    int idx = peer_find_locked(tbl, uuid);
    if (idx < 0) {
        for (int i = 0; i < PARALLAX_MAX_PEERS; ++i) {
            if (!tbl->entries[i].in_use) { idx = i; break; }
        }
        if (idx < 0) {
            pthread_rwlock_unlock(&tbl->lock);
            return -1;
        }
        memcpy(tbl->entries[idx].uuid, uuid, PARALLAX_UUID_LEN);
        tbl->entries[idx].in_use = 1;
        tbl->count++;
    }

    strncpy(tbl->entries[idx].ip, ip, INET_ADDRSTRLEN - 1);
    tbl->entries[idx].ip[INET_ADDRSTRLEN - 1] = '\0';
    tbl->entries[idx].tcp_port     = tcp_port;
    tbl->entries[idx].udp_port     = udp_port;
    tbl->entries[idx].last_seen_ms = net_protocol_now_ms();

    pthread_rwlock_unlock(&tbl->lock);
    return 0;
}

static int peer_table_lookup(peer_table_t *tbl, const uint8_t *uuid,
                             peer_entry_t *out)
{
    pthread_rwlock_rdlock(&tbl->lock);
    int idx = peer_find_locked(tbl, uuid);
    int rc = -1;
    if (idx >= 0) {
        memcpy(out, &tbl->entries[idx], sizeof(*out));
        rc = 0;
    }
    pthread_rwlock_unlock(&tbl->lock);
    return rc;
}

static int peer_table_touch(peer_table_t *tbl, const uint8_t *uuid)
{
    pthread_rwlock_wrlock(&tbl->lock);
    int idx = peer_find_locked(tbl, uuid);
    int rc = -1;
    if (idx >= 0) {
        tbl->entries[idx].last_seen_ms = net_protocol_now_ms();
        rc = 0;
    }
    pthread_rwlock_unlock(&tbl->lock);
    return rc;
}

/* ============================================================================
 * ===== PARTIE 4 - RETRY QUEUE =====
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * 4.1  I/O fiables
 * -------------------------------------------------------------------------- */

static ssize_t io_read_full(int fd, void *buf, size_t len)
{
    size_t total = 0;
    uint8_t *p = (uint8_t *)buf;
    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n == 0) return (ssize_t)total;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static ssize_t io_write_full(int fd, const void *buf, size_t len)
{
    size_t total = 0;
    const uint8_t *p = (const uint8_t *)buf;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* ----------------------------------------------------------------------------
 * 4.2  Backoff exponentiel : base * 2^(attempts-1), plafonne a 60 s
 * -------------------------------------------------------------------------- */

static uint64_t compute_next_try(int attempts, int base_backoff_ms)
{
    uint64_t now = net_protocol_now_ms();
    uint64_t backoff = (uint64_t)base_backoff_ms;
    for (int i = 1; i < attempts && backoff < 60000; ++i) backoff *= 2;
    if (backoff > 60000) backoff = 60000;
    return now + backoff;
}

/* ----------------------------------------------------------------------------
 * 4.3  Replay au demarrage
 * -------------------------------------------------------------------------- */

static int retry_queue_replay(retry_queue_t *q)
{
    int fd = open(q->path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    while (1) {
        uint32_t attempts_le, frame_len_le;
        uint64_t next_try_le;

        ssize_t r = io_read_full(fd, &attempts_le, sizeof(attempts_le));
        if (r == 0) break;
        if (r != (ssize_t)sizeof(attempts_le)) break;

        if (io_read_full(fd, &next_try_le,  sizeof(next_try_le))  != (ssize_t)sizeof(next_try_le))  break;
        if (io_read_full(fd, &frame_len_le, sizeof(frame_len_le)) != (ssize_t)sizeof(frame_len_le)) break;

        uint32_t frame_len = le32toh(frame_len_le);
        if (frame_len == 0 || frame_len > PARALLAX_MAX_PAYLOAD + PARALLAX_HEADER_SIZE) break;

        uint8_t *frame = (uint8_t *)malloc(frame_len);
        if (!frame) break;
        if (io_read_full(fd, frame, frame_len) != (ssize_t)frame_len) {
            free(frame);
            break;
        }

        if (q->count < RETRY_MAX_ENTRIES) {
            for (int i = 0; i < RETRY_MAX_ENTRIES; ++i) {
                if (!q->entries[i].in_use) {
                    q->entries[i].attempts    = le32toh(attempts_le);
                    q->entries[i].next_try_ms = le64toh(next_try_le);
                    q->entries[i].frame       = frame;
                    q->entries[i].frame_len   = frame_len;
                    q->entries[i].in_use      = 1;
                    q->entries[i].delivered   = 0;
                    q->count++;
                    frame = NULL;
                    break;
                }
            }
        }
        if (frame) free(frame);
    }

    close(fd);
    return 0;
}

/* ----------------------------------------------------------------------------
 * 4.4  Append disque
 * -------------------------------------------------------------------------- */

static int retry_queue_append_to_disk(retry_queue_t *q, uint32_t attempts,
                                      uint64_t next_try_ms,
                                      const uint8_t *frame, uint32_t frame_len)
{
    if (q->fd < 0) return -1;

    uint32_t a_le = htole32(attempts);
    uint64_t n_le = htole64(next_try_ms);
    uint32_t l_le = htole32(frame_len);

    if (io_write_full(q->fd, &a_le, sizeof(a_le)) < 0) return -1;
    if (io_write_full(q->fd, &n_le, sizeof(n_le)) < 0) return -1;
    if (io_write_full(q->fd, &l_le, sizeof(l_le)) < 0) return -1;
    if (io_write_full(q->fd, frame, frame_len)    < 0) return -1;

    fdatasync(q->fd);
    return 0;
}

/* ----------------------------------------------------------------------------
 * 4.5  API publique : init / push / pick / mark / compact / destroy
 * -------------------------------------------------------------------------- */

static int retry_queue_init(retry_queue_t *q, const char *path,
                            int max_attempts, int base_backoff_ms)
{
    memset(q, 0, sizeof(*q));
    q->fd = -1;
    q->max_attempts    = max_attempts > 0 ? max_attempts : 5;
    q->base_backoff_ms = base_backoff_ms > 0 ? base_backoff_ms : 500;
    strncpy(q->path, path, sizeof(q->path) - 1);

    if (pthread_mutex_init(&q->lock, NULL) != 0) return -1;

    /* Cree le repertoire parent si necessaire (best-effort). */
    char dir[256];
    strncpy(dir, path, sizeof(dir) - 1);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (strlen(dir) > 0) mkdir(dir, 0755);
    }

    retry_queue_replay(q);

    q->fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (q->fd < 0) {
        fprintf(stderr, "[retry] cannot open journal %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int retry_queue_push(retry_queue_t *q, uint8_t *frame, uint32_t frame_len)
{
    if (!q || !frame || frame_len == 0) return -1;

    pthread_mutex_lock(&q->lock);
    int idx = -1;
    for (int i = 0; i < RETRY_MAX_ENTRIES; ++i) {
        if (!q->entries[i].in_use) { idx = i; break; }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&q->lock);
        free(frame);
        return -1;
    }

    uint64_t next_try = compute_next_try(1, q->base_backoff_ms);
    q->entries[idx].attempts    = 0;
    q->entries[idx].next_try_ms = next_try;
    q->entries[idx].frame       = frame;
    q->entries[idx].frame_len   = frame_len;
    q->entries[idx].in_use      = 1;
    q->entries[idx].delivered   = 0;
    q->count++;

    retry_queue_append_to_disk(q, 0, next_try, frame, frame_len);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static int retry_queue_pick(retry_queue_t *q, uint8_t **out_frame, uint32_t *out_len)
{
    if (!q || !out_frame || !out_len) return -1;

    pthread_mutex_lock(&q->lock);
    uint64_t now = net_protocol_now_ms();

    int found = -1;
    for (int i = 0; i < RETRY_MAX_ENTRIES; ++i) {
        if (q->entries[i].in_use && !q->entries[i].delivered &&
            q->entries[i].next_try_ms <= now) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    *out_len = q->entries[found].frame_len;
    *out_frame = (uint8_t *)malloc(*out_len);
    if (!*out_frame) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    memcpy(*out_frame, q->entries[found].frame, *out_len);
    pthread_mutex_unlock(&q->lock);
    return found;
}

static void retry_queue_mark_attempt(retry_queue_t *q, int idx, int success)
{
    if (!q || idx < 0 || idx >= RETRY_MAX_ENTRIES) return;

    pthread_mutex_lock(&q->lock);
    if (!q->entries[idx].in_use) {
        pthread_mutex_unlock(&q->lock);
        return;
    }

    q->entries[idx].attempts++;
    if (success) {
        q->entries[idx].delivered = 1;
    } else if (q->entries[idx].attempts >= (uint32_t)q->max_attempts) {
        fprintf(stderr, "[retry] giving up on entry %d after %u attempts\n",
                idx, q->entries[idx].attempts);
        q->entries[idx].delivered = 1;
    } else {
        q->entries[idx].next_try_ms =
            compute_next_try(q->entries[idx].attempts, q->base_backoff_ms);
    }
    pthread_mutex_unlock(&q->lock);
}

static int retry_queue_compact(retry_queue_t *q)
{
    if (!q) return -1;
    pthread_mutex_lock(&q->lock);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", q->path);

    int tmp_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    int kept = 0, dropped = 0;
    for (int i = 0; i < RETRY_MAX_ENTRIES; ++i) {
        if (!q->entries[i].in_use) continue;

        if (q->entries[i].delivered) {
            free(q->entries[i].frame);
            memset(&q->entries[i], 0, sizeof(q->entries[i]));
            q->count--;
            dropped++;
            continue;
        }

        uint32_t a_le = htole32(q->entries[i].attempts);
        uint64_t n_le = htole64(q->entries[i].next_try_ms);
        uint32_t l_le = htole32(q->entries[i].frame_len);

        if (io_write_full(tmp_fd, &a_le, sizeof(a_le)) < 0 ||
            io_write_full(tmp_fd, &n_le, sizeof(n_le)) < 0 ||
            io_write_full(tmp_fd, &l_le, sizeof(l_le)) < 0 ||
            io_write_full(tmp_fd, q->entries[i].frame, q->entries[i].frame_len) < 0) {
            close(tmp_fd);
            unlink(tmp_path);
            pthread_mutex_unlock(&q->lock);
            return -1;
        }
        kept++;
    }

    fdatasync(tmp_fd);
    close(tmp_fd);

    if (q->fd >= 0) close(q->fd);

    if (rename(tmp_path, q->path) != 0) {
        q->fd = open(q->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        pthread_mutex_unlock(&q->lock);
        return -1;
    }

    q->fd = open(q->path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    fprintf(stderr, "[retry] compaction: kept=%d, dropped=%d\n", kept, dropped);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static void retry_queue_destroy(retry_queue_t *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < RETRY_MAX_ENTRIES; ++i) {
        if (q->entries[i].in_use && q->entries[i].frame) free(q->entries[i].frame);
    }
    if (q->fd >= 0) close(q->fd);
    pthread_mutex_unlock(&q->lock);
    pthread_mutex_destroy(&q->lock);
    memset(q, 0, sizeof(*q));
    q->fd = -1;
}

/* ============================================================================
 * ===== PARTIE 5 - NETWORK CORE =====
 * ============================================================================ */

/* ----------------------------------------------------------------------------
 * 5.1  Helpers internes (log, fcntl, slots de connexion)
 * -------------------------------------------------------------------------- */

static void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[net] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_reuseaddr(int fd)
{
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
}

static int set_broadcast(int fd)
{
    int yes = 1;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
}

static int conn_find_free(net_state_t *st)
{
    for (int i = 0; i < NET_MAX_TCP_CONNS; ++i) {
        if (!st->tcp_conns[i].in_use) return i;
    }
    return -1;
}

static int conn_find_by_fd(net_state_t *st, int fd)
{
    for (int i = 0; i < NET_MAX_TCP_CONNS; ++i) {
        if (st->tcp_conns[i].in_use && st->tcp_conns[i].fd == fd) return i;
    }
    return -1;
}

static void conn_close(net_state_t *st, int idx)
{
    if (idx < 0 || idx >= NET_MAX_TCP_CONNS) return;
    if (!st->tcp_conns[idx].in_use) return;

    epoll_ctl(st->epoll_fd, EPOLL_CTL_DEL, st->tcp_conns[idx].fd, NULL);
    close(st->tcp_conns[idx].fd);
    free(st->tcp_conns[idx].rx_buf);
    memset(&st->tcp_conns[idx], 0, sizeof(st->tcp_conns[idx]));
}

/* ----------------------------------------------------------------------------
 * 5.2  Initialisation : sockets, queues, epoll
 * -------------------------------------------------------------------------- */

static int setup_tcp_listener(net_state_t *st)
{
    st->tcp_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (st->tcp_listen_fd < 0) {
        log_info("socket(TCP) failed: %s", strerror(errno));
        return -1;
    }
    set_reuseaddr(st->tcp_listen_fd);
    set_nonblocking(st->tcp_listen_fd);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(st->cfg.tcp_port);

    if (bind(st->tcp_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_info("bind(TCP %d) failed: %s", st->cfg.tcp_port, strerror(errno));
        return -1;
    }
    if (listen(st->tcp_listen_fd, 16) < 0) {
        log_info("listen() failed: %s", strerror(errno));
        return -1;
    }
    log_info("TCP listening on port %d", st->cfg.tcp_port);
    return 0;
}

static int setup_udp_socket(net_state_t *st)
{
    st->udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (st->udp_fd < 0) {
        log_info("socket(UDP) failed: %s", strerror(errno));
        return -1;
    }
    set_reuseaddr(st->udp_fd);
    set_broadcast(st->udp_fd);
    set_nonblocking(st->udp_fd);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(st->cfg.udp_port);

    if (bind(st->udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_info("bind(UDP %d) failed: %s", st->cfg.udp_port, strerror(errno));
        return -1;
    }
    log_info("UDP listening on port %d (broadcast %s)",
             st->cfg.udp_port, st->cfg.broadcast_addr);
    return 0;
}

static int setup_qpairs(net_state_t *st)
{
    st->num_qpairs = 0;

    for (int i = 0; i < st->cfg.num_services && st->num_qpairs < PARALLAX_MAX_SERVICES; ++i) {
        service_id_t svc = st->cfg.services[i];
        service_qpair_t *qp = &st->qpairs[st->num_qpairs];
        qp->id = svc;

        net_config_queue_name(svc, 1, qp->name_in,  sizeof(qp->name_in));
        net_config_queue_name(svc, 0, qp->name_out, sizeof(qp->name_out));

        struct mq_attr attr = {0};
        attr.mq_maxmsg  = 10;
        attr.mq_msgsize = 8192;

        mq_unlink(qp->name_in);
        mq_unlink(qp->name_out);

        qp->mq_in = mq_open(qp->name_in, O_CREAT | O_WRONLY | O_NONBLOCK, 0644, &attr);
        if (qp->mq_in == (mqd_t)-1) {
            log_info("mq_open(%s) failed: %s", qp->name_in, strerror(errno));
            return -1;
        }
        qp->mq_out = mq_open(qp->name_out, O_CREAT | O_RDONLY, 0644, &attr);
        if (qp->mq_out == (mqd_t)-1) {
            log_info("mq_open(%s) failed: %s", qp->name_out, strerror(errno));
            return -1;
        }

        log_info("service %s queues ready (in=%s out=%s)",
                 net_config_service_to_name(svc), qp->name_in, qp->name_out);
        st->num_qpairs++;
    }
    return 0;
}

static int net_core_init(net_state_t *st, const char *config_path)
{
    memset(st, 0, sizeof(*st));

    if (net_config_load(config_path, &st->cfg) != 0) return -1;

    char uuid_hex[33];
    uuid_to_hex(st->cfg.uuid, uuid_hex);
    log_info("starting node uuid=%s role=%s", uuid_hex, st->cfg.role);

    if (peer_table_init(&st->peers) != 0) return -1;
    if (retry_queue_init(&st->retry, st->cfg.retry_path,
                         st->cfg.retry_max_attempts,
                         st->cfg.retry_base_backoff_ms) != 0) return -1;

    if (setup_tcp_listener(st) != 0) return -1;
    if (setup_udp_socket(st)   != 0) return -1;
    if (setup_qpairs(st)       != 0) return -1;

    st->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (st->epoll_fd < 0) {
        log_info("epoll_create1 failed: %s", strerror(errno));
        return -1;
    }

    struct epoll_event ev = {0};
    ev.events  = EPOLLIN;
    ev.data.fd = st->tcp_listen_fd;
    epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->tcp_listen_fd, &ev);

    ev.data.fd = st->udp_fd;
    epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, st->udp_fd, &ev);

    st->running = 1;
    return 0;
}

/* ----------------------------------------------------------------------------
 * 5.3  HELLO (decouverte des pairs)
 *
 * Payload d'un HELLO : [u16 tcp_port][u16 udp_port][16 octets reserves]
 * L'IP est lue cote receveur via recvfrom().
 * -------------------------------------------------------------------------- */

static int build_hello_payload(net_state_t *st, uint8_t **out_buf, uint32_t *out_len)
{
    uint32_t len = 2 + 2 + 16;
    uint8_t *buf = (uint8_t *)calloc(1, len);
    if (!buf) return -1;

    uint16_t tcp_le = htole16(st->cfg.tcp_port);
    uint16_t udp_le = htole16(st->cfg.udp_port);
    memcpy(buf + 0, &tcp_le, 2);
    memcpy(buf + 2, &udp_le, 2);

    *out_buf = buf;
    *out_len = len;
    return 0;
}

static void parse_hello_payload(const uint8_t *payload, uint32_t len,
                                uint16_t *tcp_port, uint16_t *udp_port)
{
    if (len < 4) {
        *tcp_port = 0;
        *udp_port = 0;
        return;
    }
    uint16_t tcp_le, udp_le;
    memcpy(&tcp_le, payload + 0, 2);
    memcpy(&udp_le, payload + 2, 2);
    *tcp_port = le16toh(tcp_le);
    *udp_port = le16toh(udp_le);
}

static int net_core_broadcast_hello(net_state_t *st)
{
    net_packet_t pkt = {0};
    pkt.header.magic        = PARALLAX_MAGIC;
    pkt.header.version      = PARALLAX_PROTO_VERSION;
    pkt.header.pkt_type     = PKT_HELLO;
    pkt.header.service_id   = SVC_NETWORK;
    pkt.header.flags        = PKT_FLAG_BROADCAST;
    memcpy(pkt.header.src_uuid, st->cfg.uuid, PARALLAX_UUID_LEN);
    pkt.header.pkt_id       = net_protocol_next_pkt_id();
    pkt.header.timestamp_ms = net_protocol_now_ms();

    uint8_t *payload = NULL;
    uint32_t plen = 0;
    if (build_hello_payload(st, &payload, &plen) != 0) return -1;

    pkt.payload            = payload;
    pkt.header.payload_len = plen;
    pkt.header.crc32       = net_protocol_crc32(payload, plen);

    uint8_t header_buf[PARALLAX_HEADER_SIZE];
    net_protocol_pack_header(&pkt.header, header_buf);

    uint8_t *frame = (uint8_t *)malloc(PARALLAX_HEADER_SIZE + plen);
    if (!frame) { free(payload); return -1; }
    memcpy(frame, header_buf, PARALLAX_HEADER_SIZE);
    memcpy(frame + PARALLAX_HEADER_SIZE, payload, plen);

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(st->cfg.udp_port);
    inet_pton(AF_INET, st->cfg.broadcast_addr, &dst.sin_addr);

    ssize_t n = sendto(st->udp_fd, frame, PARALLAX_HEADER_SIZE + plen, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    int rc = (n >= 0) ? 0 : -1;
    if (rc < 0) log_info("HELLO broadcast failed: %s", strerror(errno));
    else        log_info("HELLO broadcast sent");

    free(frame);
    free(payload);
    return rc;
}

static void handle_hello(net_state_t *st, const net_packet_t *pkt,
                         const struct sockaddr_in *from)
{
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, ip_str, sizeof(ip_str));

    uint16_t tcp_port, udp_port;
    parse_hello_payload(pkt->payload, pkt->header.payload_len, &tcp_port, &udp_port);

    if (uuid_equal(pkt->header.src_uuid, st->cfg.uuid)) return;

    peer_table_upsert(&st->peers, pkt->header.src_uuid, ip_str, tcp_port, udp_port);

    char hex[33];
    uuid_to_hex(pkt->header.src_uuid, hex);
    log_info("HELLO from %s @ %s:%u (tcp) / %u (udp)", hex, ip_str, tcp_port, udp_port);

    if (strcmp(st->cfg.role, "controller") == 0) {
        net_packet_t ack = {0};
        ack.header.magic        = PARALLAX_MAGIC;
        ack.header.version      = PARALLAX_PROTO_VERSION;
        ack.header.pkt_type     = PKT_HELLO_ACK;
        ack.header.service_id   = SVC_NETWORK;
        memcpy(ack.header.src_uuid, st->cfg.uuid, PARALLAX_UUID_LEN);
        memcpy(ack.header.dst_uuid, pkt->header.src_uuid, PARALLAX_UUID_LEN);
        ack.header.pkt_id       = net_protocol_next_pkt_id();
        ack.header.timestamp_ms = net_protocol_now_ms();
        ack.header.payload_len  = 0;
        ack.header.crc32        = 0;

        uint8_t hbuf[PARALLAX_HEADER_SIZE];
        net_protocol_pack_header(&ack.header, hbuf);
        sendto(st->udp_fd, hbuf, PARALLAX_HEADER_SIZE, 0,
               (struct sockaddr *)from, sizeof(*from));
        log_info("HELLO_ACK sent to %s", hex);
    }
}

/* ----------------------------------------------------------------------------
 * 5.4  Envoi (TCP/UDP + retry sur echec)
 * -------------------------------------------------------------------------- */

static uint8_t *serialize_packet(const net_packet_t *pkt, uint32_t *out_len)
{
    uint32_t total = PARALLAX_HEADER_SIZE + pkt->header.payload_len;
    uint8_t *frame = (uint8_t *)malloc(total);
    if (!frame) return NULL;

    if (net_protocol_pack_header(&pkt->header, frame) != 0) {
        free(frame);
        return NULL;
    }
    if (pkt->header.payload_len > 0 && pkt->payload) {
        memcpy(frame + PARALLAX_HEADER_SIZE, pkt->payload, pkt->header.payload_len);
    }
    *out_len = total;
    return frame;
}

static int packet_uses_udp(uint8_t pkt_type)
{
    switch (pkt_type) {
        case PKT_HELLO:
        case PKT_HELLO_ACK:
        case PKT_HEARTBEAT:
        case PKT_GOSSIP:
        case PKT_PING:
        case PKT_PONG:
            return 1;
        default:
            return 0;
    }
}

static int find_or_open_tcp_conn(net_state_t *st, const peer_entry_t *peer)
{
    for (int i = 0; i < NET_MAX_TCP_CONNS; ++i) {
        if (st->tcp_conns[i].in_use &&
            strcmp(st->tcp_conns[i].peer_ip, peer->ip) == 0 &&
            st->tcp_conns[i].peer_port == peer->tcp_port) {
            return i;
        }
    }

    int idx = conn_find_free(st);
    if (idx < 0) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(peer->tcp_port);
    inet_pton(AF_INET, peer->ip, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_info("connect to %s:%u failed: %s", peer->ip, peer->tcp_port, strerror(errno));
        close(fd);
        return -1;
    }
    set_nonblocking(fd);

    st->tcp_conns[idx].fd        = fd;
    st->tcp_conns[idx].in_use    = 1;
    st->tcp_conns[idx].peer_port = peer->tcp_port;
    strncpy(st->tcp_conns[idx].peer_ip, peer->ip, INET_ADDRSTRLEN - 1);
    st->tcp_conns[idx].rx_buf     = NULL;
    st->tcp_conns[idx].rx_buf_cap = 0;
    st->tcp_conns[idx].rx_buf_len = 0;

    struct epoll_event ev = {0};
    ev.events  = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

    log_info("TCP connected to %s:%u", peer->ip, peer->tcp_port);
    return idx;
}

static int send_udp(net_state_t *st, const net_packet_t *pkt,
                    const uint8_t *frame, uint32_t flen)
{
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;

    if (uuid_is_zero(pkt->header.dst_uuid) ||
        (pkt->header.flags & PKT_FLAG_BROADCAST)) {
        dst.sin_port = htons(st->cfg.udp_port);
        inet_pton(AF_INET, st->cfg.broadcast_addr, &dst.sin_addr);
    } else {
        peer_entry_t peer;
        if (peer_table_lookup(&st->peers, pkt->header.dst_uuid, &peer) != 0) {
            return -1;
        }
        dst.sin_port = htons(peer.udp_port);
        inet_pton(AF_INET, peer.ip, &dst.sin_addr);
    }

    ssize_t n = sendto(st->udp_fd, frame, flen, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    return (n >= 0) ? 0 : -1;
}

static int send_tcp(net_state_t *st, const net_packet_t *pkt,
                    const uint8_t *frame, uint32_t flen)
{
    if (uuid_is_zero(pkt->header.dst_uuid)) {
        log_info("TCP packet without dst_uuid, dropping");
        return -1;
    }

    peer_entry_t peer;
    if (peer_table_lookup(&st->peers, pkt->header.dst_uuid, &peer) != 0) return -1;

    int idx = find_or_open_tcp_conn(st, &peer);
    if (idx < 0) return -1;

    size_t total = 0;
    while (total < flen) {
        ssize_t n = send(st->tcp_conns[idx].fd, frame + total, flen - total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
                nanosleep(&ts, NULL);
                continue;
            }
            log_info("TCP send to %s:%u failed: %s",
                     peer.ip, peer.tcp_port, strerror(errno));
            conn_close(st, idx);
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int net_core_send_packet(net_state_t *st, net_packet_t *pkt)
{
    if (!st || !pkt) return -1;

    if (pkt->header.payload_len > 0 && pkt->payload && pkt->header.crc32 == 0) {
        pkt->header.crc32 = net_protocol_crc32(pkt->payload, pkt->header.payload_len);
    }

    uint32_t flen = 0;
    uint8_t *frame = serialize_packet(pkt, &flen);
    if (!frame) {
        free(pkt->payload);
        return -1;
    }

    int rc;
    if (packet_uses_udp(pkt->header.pkt_type)) rc = send_udp(st, pkt, frame, flen);
    else                                       rc = send_tcp(st, pkt, frame, flen);

    if (rc != 0 && !packet_uses_udp(pkt->header.pkt_type)) {
        log_info("queueing pkt_id=%lu for retry", (unsigned long)pkt->header.pkt_id);
        retry_queue_push(&st->retry, frame, flen);
    } else {
        free(frame);
    }

    if (pkt->payload) {
        free(pkt->payload);
        pkt->payload = NULL;
    }
    return rc;
}

/* ----------------------------------------------------------------------------
 * 5.5  Dispatcher : paquet recu -> queue _in du service local
 * -------------------------------------------------------------------------- */

static service_qpair_t *find_qpair(net_state_t *st, service_id_t svc)
{
    for (int i = 0; i < st->num_qpairs; ++i) {
        if (st->qpairs[i].id == svc) return &st->qpairs[i];
    }
    return NULL;
}

static int dispatch_to_local_service(net_state_t *st, const net_packet_t *pkt)
{
    service_qpair_t *qp = find_qpair(st, (service_id_t)pkt->header.service_id);
    if (!qp) {
        log_info("no local service for service_id=%u (pkt_type=%u dropped)",
                 pkt->header.service_id, pkt->header.pkt_type);
        return -1;
    }

    size_t total_len = PARALLAX_HEADER_SIZE + pkt->header.payload_len;

    struct mq_attr attr;
    if (mq_getattr(qp->mq_in, &attr) != 0) return -1;

    if (total_len > (size_t)attr.mq_msgsize) {
        log_info("packet too large for mq (%zu > %ld), truncating",
                 total_len, attr.mq_msgsize);
        total_len = (size_t)attr.mq_msgsize;
    }

    uint8_t *buf = (uint8_t *)malloc(total_len);
    if (!buf) return -1;

    if (net_protocol_pack_header(&pkt->header, buf) != 0) {
        free(buf);
        return -1;
    }

    size_t copy_len = total_len - PARALLAX_HEADER_SIZE;
    if (copy_len > pkt->header.payload_len) copy_len = pkt->header.payload_len;
    if (copy_len > 0 && pkt->payload) {
        memcpy(buf + PARALLAX_HEADER_SIZE, pkt->payload, copy_len);
    }

    int rc = mq_send(qp->mq_in, (const char *)buf, total_len, 0);
    if (rc < 0) {
        if (errno == EAGAIN) {
            log_info("queue _in full for %s, dropping packet",
                     net_config_service_to_name(qp->id));
        } else {
            log_info("mq_send to %s failed: %s",
                     net_config_service_to_name(qp->id), strerror(errno));
        }
    }
    free(buf);
    return rc;
}

/* ----------------------------------------------------------------------------
 * 5.6  Reception UDP et TCP
 * -------------------------------------------------------------------------- */

static void handle_udp_recv(net_state_t *st)
{
    while (1) {
        uint8_t buf[65536];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);

        ssize_t n = recvfrom(st->udp_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            log_info("recvfrom UDP failed: %s", strerror(errno));
            break;
        }
        if (n < (ssize_t)PARALLAX_HEADER_SIZE) {
            log_info("UDP datagram too small (%zd bytes)", n);
            continue;
        }

        net_packet_t pkt = {0};
        if (net_protocol_unpack_header(buf, &pkt.header) != 0) {
            log_info("invalid UDP header from %s", inet_ntoa(from.sin_addr));
            continue;
        }
        if ((size_t)n < PARALLAX_HEADER_SIZE + pkt.header.payload_len) {
            log_info("UDP payload truncated");
            continue;
        }

        if (pkt.header.payload_len > 0) {
            pkt.payload = (uint8_t *)malloc(pkt.header.payload_len);
            if (!pkt.payload) continue;
            memcpy(pkt.payload, buf + PARALLAX_HEADER_SIZE, pkt.header.payload_len);
        }

        if (net_protocol_verify(&pkt) != 0) {
            log_info("UDP packet verify failed (CRC mismatch?)");
            net_protocol_free_packet(&pkt);
            continue;
        }

        peer_table_touch(&st->peers, pkt.header.src_uuid);

        switch (pkt.header.pkt_type) {
            case PKT_HELLO:
                handle_hello(st, &pkt, &from);
                break;
            case PKT_HELLO_ACK: {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                peer_table_upsert(&st->peers, pkt.header.src_uuid, ip,
                                  st->cfg.tcp_port, st->cfg.udp_port);
                char hex[33]; uuid_to_hex(pkt.header.src_uuid, hex);
                log_info("HELLO_ACK from %s @ %s", hex, ip);
                break;
            }
            default:
                dispatch_to_local_service(st, &pkt);
                break;
        }
        net_protocol_free_packet(&pkt);
    }
}

static void handle_tcp_accept(net_state_t *st)
{
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int fd = accept(st->tcp_listen_fd, (struct sockaddr *)&from, &fromlen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            log_info("accept failed: %s", strerror(errno));
            break;
        }
        set_nonblocking(fd);

        int idx = conn_find_free(st);
        if (idx < 0) {
            log_info("too many TCP connections, dropping");
            close(fd);
            continue;
        }
        st->tcp_conns[idx].fd        = fd;
        st->tcp_conns[idx].in_use    = 1;
        st->tcp_conns[idx].peer_port = ntohs(from.sin_port);
        inet_ntop(AF_INET, &from.sin_addr, st->tcp_conns[idx].peer_ip, INET_ADDRSTRLEN);
        st->tcp_conns[idx].rx_buf     = NULL;
        st->tcp_conns[idx].rx_buf_cap = 0;
        st->tcp_conns[idx].rx_buf_len = 0;

        struct epoll_event ev = {0};
        ev.events  = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(st->epoll_fd, EPOLL_CTL_ADD, fd, &ev);

        log_info("TCP accepted from %s:%u",
                 st->tcp_conns[idx].peer_ip, st->tcp_conns[idx].peer_port);
    }
}

static void handle_tcp_data(net_state_t *st, int conn_idx)
{
    tcp_conn_t *c = &st->tcp_conns[conn_idx];

    while (1) {
        size_t needed = c->rx_buf_len + 4096;
        if (needed > c->rx_buf_cap) {
            size_t new_cap = c->rx_buf_cap ? c->rx_buf_cap * 2 : 8192;
            while (new_cap < needed) new_cap *= 2;
            if (new_cap > PARALLAX_MAX_PAYLOAD + PARALLAX_HEADER_SIZE + 64) {
                log_info("TCP buffer overflow on %s, closing", c->peer_ip);
                conn_close(st, conn_idx);
                return;
            }
            uint8_t *nb = (uint8_t *)realloc(c->rx_buf, new_cap);
            if (!nb) { conn_close(st, conn_idx); return; }
            c->rx_buf     = nb;
            c->rx_buf_cap = new_cap;
        }

        ssize_t n = recv(c->fd, c->rx_buf + c->rx_buf_len,
                         c->rx_buf_cap - c->rx_buf_len, 0);
        if (n == 0) {
            log_info("TCP closed by peer %s", c->peer_ip);
            conn_close(st, conn_idx);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            log_info("recv from %s failed: %s", c->peer_ip, strerror(errno));
            conn_close(st, conn_idx);
            return;
        }
        c->rx_buf_len += (size_t)n;

        while (c->rx_buf_len >= PARALLAX_HEADER_SIZE) {
            net_header_t hdr;
            if (net_protocol_unpack_header(c->rx_buf, &hdr) != 0) {
                log_info("invalid TCP header from %s, closing", c->peer_ip);
                conn_close(st, conn_idx);
                return;
            }
            size_t total = PARALLAX_HEADER_SIZE + hdr.payload_len;
            if (c->rx_buf_len < total) break;

            net_packet_t pkt = {0};
            pkt.header = hdr;
            if (hdr.payload_len > 0) {
                pkt.payload = (uint8_t *)malloc(hdr.payload_len);
                if (pkt.payload) {
                    memcpy(pkt.payload, c->rx_buf + PARALLAX_HEADER_SIZE, hdr.payload_len);
                }
            }

            if (net_protocol_verify(&pkt) == 0) {
                peer_table_touch(&st->peers, pkt.header.src_uuid);
                dispatch_to_local_service(st, &pkt);
            } else {
                log_info("TCP packet verify failed from %s", c->peer_ip);
            }
            net_protocol_free_packet(&pkt);

            memmove(c->rx_buf, c->rx_buf + total, c->rx_buf_len - total);
            c->rx_buf_len -= total;
        }
    }
}

/* ----------------------------------------------------------------------------
 * 5.7  Thread sondeur des queues _out
 *
 * mq_receive ne s'integre pas a epoll sur Linux (mq_notify est complexe).
 * Un thread dedie qui boucle en mq_timedreceive sur chaque queue _out est
 * plus simple. Quand un message arrive, on l'envoie sur le reseau.
 * -------------------------------------------------------------------------- */

static void *outq_thread_fn(void *arg)
{
    net_state_t *st = (net_state_t *)arg;
    log_info("outq thread started");

    while (st->running) {
        int activity = 0;

        for (int i = 0; i < st->num_qpairs && st->running; ++i) {
            service_qpair_t *qp = &st->qpairs[i];

            uint8_t buf[8192];
            unsigned int prio = 0;

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50 * 1000000;       /* +50 ms */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_nsec -= 1000000000;
                ts.tv_sec  += 1;
            }

            ssize_t n = mq_timedreceive(qp->mq_out, (char *)buf, sizeof(buf), &prio, &ts);
            if (n < 0) {
                if (errno == ETIMEDOUT) continue;
                if (errno == EINTR)     continue;
                log_info("mq_timedreceive(%s) failed: %s", qp->name_out, strerror(errno));
                continue;
            }
            activity = 1;

            if ((size_t)n < PARALLAX_HEADER_SIZE) {
                log_info("outgoing message too small on %s", qp->name_out);
                continue;
            }

            net_packet_t pkt = {0};
            if (net_protocol_unpack_header(buf, &pkt.header) != 0) {
                log_info("bad outgoing header on %s", qp->name_out);
                continue;
            }
            if (uuid_is_zero(pkt.header.src_uuid)) {
                memcpy(pkt.header.src_uuid, st->cfg.uuid, PARALLAX_UUID_LEN);
            }
            if (pkt.header.timestamp_ms == 0) pkt.header.timestamp_ms = net_protocol_now_ms();
            if (pkt.header.pkt_id == 0)       pkt.header.pkt_id       = net_protocol_next_pkt_id();

            if (pkt.header.payload_len > 0) {
                if ((size_t)n < PARALLAX_HEADER_SIZE + pkt.header.payload_len) {
                    log_info("outgoing payload truncated on %s", qp->name_out);
                    continue;
                }
                pkt.payload = (uint8_t *)malloc(pkt.header.payload_len);
                if (!pkt.payload) continue;
                memcpy(pkt.payload, buf + PARALLAX_HEADER_SIZE, pkt.header.payload_len);
            }

            net_core_send_packet(st, &pkt);
        }

        if (!activity) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }
    log_info("outq thread stopped");
    return NULL;
}

/* ----------------------------------------------------------------------------
 * 5.8  Cycle de vie : run / stop / shutdown
 * -------------------------------------------------------------------------- */

static void net_core_stop(net_state_t *st)
{
    if (st) st->running = 0;
}

static void process_retry_queue(net_state_t *st)
{
    while (1) {
        uint8_t *frame = NULL;
        uint32_t flen  = 0;
        int idx = retry_queue_pick(&st->retry, &frame, &flen);
        if (idx < 0) break;

        net_header_t hdr;
        if (net_protocol_unpack_header(frame, &hdr) != 0) {
            free(frame);
            retry_queue_mark_attempt(&st->retry, idx, 1);
            continue;
        }

        net_packet_t pkt = {0};
        pkt.header = hdr;
        pkt.payload = NULL;

        int rc;
        if (packet_uses_udp(hdr.pkt_type)) rc = send_udp(st, &pkt, frame, flen);
        else                                rc = send_tcp(st, &pkt, frame, flen);

        free(frame);
        retry_queue_mark_attempt(&st->retry, idx, rc == 0 ? 1 : 0);

        if (rc == 0) log_info("retry succeeded for pkt_id=%lu", (unsigned long)hdr.pkt_id);
    }
}

static int net_core_run(net_state_t *st)
{
    if (!st) return -1;

    if (pthread_create(&st->outq_thread, NULL, outq_thread_fn, st) != 0) {
        log_info("pthread_create(outq) failed");
        return -1;
    }

    net_core_broadcast_hello(st);

    uint64_t last_retry_run    = net_protocol_now_ms();
    uint64_t last_compaction   = net_protocol_now_ms();
    uint64_t last_hello_resend = net_protocol_now_ms();

    while (st->running) {
        struct epoll_event events[NET_MAX_EPOLL_EVT];
        int n = epoll_wait(st->epoll_fd, events, NET_MAX_EPOLL_EVT, 200);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_info("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == st->tcp_listen_fd) {
                handle_tcp_accept(st);
            } else if (fd == st->udp_fd) {
                handle_udp_recv(st);
            } else {
                int conn_idx = conn_find_by_fd(st, fd);
                if (conn_idx >= 0) handle_tcp_data(st, conn_idx);
            }
        }

        uint64_t now = net_protocol_now_ms();
        if (now - last_retry_run    >= 500)   { process_retry_queue(st);          last_retry_run    = now; }
        if (now - last_compaction   >= 60000) { retry_queue_compact(&st->retry);  last_compaction   = now; }
        if (now - last_hello_resend >= 30000) { net_core_broadcast_hello(st);     last_hello_resend = now; }
    }

    pthread_join(st->outq_thread, NULL);
    return 0;
}

static void net_core_shutdown(net_state_t *st)
{
    if (!st) return;

    for (int i = 0; i < NET_MAX_TCP_CONNS; ++i) {
        if (st->tcp_conns[i].in_use) conn_close(st, i);
    }
    if (st->tcp_listen_fd > 0) close(st->tcp_listen_fd);
    if (st->udp_fd        > 0) close(st->udp_fd);
    if (st->epoll_fd      > 0) close(st->epoll_fd);

    for (int i = 0; i < st->num_qpairs; ++i) {
        if (st->qpairs[i].mq_in  != (mqd_t)-1) {
            mq_close(st->qpairs[i].mq_in);
            mq_unlink(st->qpairs[i].name_in);
        }
        if (st->qpairs[i].mq_out != (mqd_t)-1) {
            mq_close(st->qpairs[i].mq_out);
            mq_unlink(st->qpairs[i].name_out);
        }
    }

    retry_queue_destroy(&st->retry);
    peer_table_destroy(&st->peers);

    log_info("shutdown complete");
}

/* ============================================================================
 * ===== PARTIE 6 - MAIN =====
 * ============================================================================ */

static net_state_t g_state;

static void on_signal(int sig)
{
    (void)sig;
    /* Pas de log dans un signal handler (non async-signal-safe). */
    net_core_stop(&g_state);
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <config.ini>\n", argv[0]);
        return 1;
    }

    /* SIGPIPE ignore : on detecte les peers casses via les codes de retour. */
    signal(SIGPIPE, SIG_IGN);

    /* Arret propre sur SIGINT et SIGTERM. */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (net_core_init(&g_state, argv[1]) != 0) {
        fprintf(stderr, "init failed\n");
        return 2;
    }

    int rc = net_core_run(&g_state);

    net_core_shutdown(&g_state);
    return rc == 0 ? 0 : 3;
}
