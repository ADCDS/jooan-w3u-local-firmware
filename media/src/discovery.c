/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "jipc.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <net/if.h>
#include <netdb.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DISCOVERY_PACKET 4096
#define DISCOVERY_NODES 64
#define DISCOVERY_INTERFACES 16
#define DISCOVERY_PEERS 32
#define DISCOVERY_GROUP "239.255.255.250"
#define DISCOVERY_2005 "http://schemas.xmlsoap.org/ws/2005/04/discovery"
#define DISCOVERY_2009 "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"
#define ADDRESSING_2004 "http://schemas.xmlsoap.org/ws/2004/08/addressing"
#define ADDRESSING_2005 "http://www.w3.org/2005/08/addressing"

static const char *const scopes[] = {
    "onvif://www.onvif.org/type/video_encoder",
    "onvif://www.onvif.org/Profile/Streaming",
    "onvif://www.onvif.org/name/jooanipc-open",
    "onvif://www.onvif.org/hardware/JA-A12"
};
typedef struct { const char *data; size_t size; } text_span;
typedef struct {
    text_span name, text, match_by;
    int parent, has_children;
} xml_node;
typedef struct {
    xml_node node[DISCOVERY_NODES];
    unsigned count;
    int header, action;
} discovery_xml;
typedef struct { struct in_addr address; uint64_t next; } peer_limit;
typedef struct {
    jipc_app *app;
    jipc_config config;
    int fd, wake[2], loopback, wildcard;
    atomic_int quitting;
    struct in_addr bound;
    struct ip_mreq membership[DISCOVERY_INTERFACES];
    unsigned memberships;
    char uuid[37], sequence[37];
    uint32_t instance, message_number;
    uint64_t input_window, output_window;
    unsigned input_count, output_count;
    peer_limit peer[DISCOVERY_PEERS];
} discovery_state;

static int whitespace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
static int name_character(unsigned char c)
{
    return isalnum(c) || c == '_' || c == ':' || c == '-' || c == '.';
}
static int span_equal(text_span a, text_span b)
{
    return a.size == b.size && !memcmp(a.data, b.data, a.size);
}
static int local_name(text_span span, const char *name)
{
    for (size_t i = 0; i < span.size; ++i) if (span.data[i] == ':') {
        span.data += i + 1; span.size -= i + 1; break;
    }
    return strlen(name) == span.size && !memcmp(span.data, name, span.size);
}
static int span_copy(text_span span, char *out, size_t capacity)
{
    while (span.size && whitespace((unsigned char)*span.data)) { ++span.data; --span.size; }
    while (span.size && whitespace((unsigned char)span.data[span.size - 1])) --span.size;
    if (span.size >= capacity) return -1;
    for (size_t i = 0; i < span.size; ++i)
        if ((unsigned char)span.data[i] < 32 && !whitespace((unsigned char)span.data[i])) return -1;
    memcpy(out, span.data, span.size); out[span.size] = 0;
    return 0;
}
static int xml_child(const discovery_xml *xml, int parent, const char *name)
{
    int found = -1;
    for (unsigned i = 0; i < xml->count; ++i)
        if (xml->node[i].parent == parent && local_name(xml->node[i].name, name)) {
            if (found >= 0) return -2;
            found = (int)i;
        }
    return found;
}
static int xml_value(const discovery_xml *xml, int parent, const char *name,
                     char *out, size_t capacity)
{
    int node = xml_child(xml, parent, name);
    if (node < 0 || xml->node[node].has_children) return -1;
    return span_copy(xml->node[node].text, out, capacity);
}
/* There is deliberately no XML entity or DTD support. Message IDs are copied
 * only after parsing and then XML escaped before they enter a response. */
static int parse_xml(const char *packet, size_t length, discovery_xml *xml)
{
    if (!length || length > DISCOVERY_PACKET || memchr(packet, 0, length) ||
        memchr(packet, '&', length)) return -1;
    const char *p = packet, *end = packet + length;
    int stack[16], depth = 0, root_seen = 0;
    memset(xml, 0, sizeof(*xml)); xml->header = xml->action = -1;
    while (p < end) {
        if (*p != '<') {
            const char *q = p; while (q < end && *q != '<') ++q;
            if (!depth) for (const char *r = p; r < q; ++r)
                if (!whitespace((unsigned char)*r)) return -1;
            p = q; continue;
        }
        if ((size_t)(end - p) >= 5 && !memcmp(p, "<?xml", 5) && !root_seen) {
            const char *q = strstr(p + 5, "?>"); if (!q || q + 2 > end) return -1;
            p = q + 2; continue;
        }
        const char *opening = p++;
        if (p == end || *p == '!' || *p == '?') return -1;
        int close = *p == '/'; if (close) ++p;
        const char *name = p; while (p < end && name_character((unsigned char)*p)) ++p;
        if (name == p || p - name > 128) return -1;
        text_span tag = {name, (size_t)(p - name)};
        if (close) {
            while (p < end && whitespace((unsigned char)*p)) ++p;
            if (!depth || p == end || *p++ != '>') return -1;
            xml_node *node = &xml->node[stack[--depth]];
            if (!span_equal(node->name, tag)) return -1;
            node->text.size = (size_t)(opening - node->text.data);
            continue;
        }
        if ((!depth && root_seen) || depth == 16 || xml->count == DISCOVERY_NODES) return -1;
        int index = (int)xml->count++;
        xml_node *node = &xml->node[index]; node->name = tag;
        node->parent = depth ? stack[depth - 1] : -1;
        if (depth) xml->node[node->parent].has_children = 1;
        else root_seen = 1;
        int empty = 0;
        unsigned attributes = 0;
        while (p < end) {
            while (p < end && whitespace((unsigned char)*p)) ++p;
            if (p == end) return -1;
            if (*p == '>') { ++p; break; }
            if (p + 1 < end && p[0] == '/' && p[1] == '>') { p += 2; empty = 1; break; }
            const char *a = p; while (p < end && name_character((unsigned char)*p)) ++p;
            if (p == a || p - a > 128 || ++attributes > 32) return -1;
            text_span attribute = {a, (size_t)(p - a)};
            while (p < end && whitespace((unsigned char)*p)) ++p;
            if (p == end || *p++ != '=') return -1;
            while (p < end && whitespace((unsigned char)*p)) ++p;
            if (p == end || (*p != '\'' && *p != '"')) return -1;
            char quote = *p++; const char *value = p;
            while (p < end && *p != quote && *p != '<') ++p;
            if (p == end || *p != quote) return -1;
            if (local_name(attribute, "MatchBy")) {
                if (node->match_by.data) return -1;
                node->match_by = (text_span){value, (size_t)(p - value)};
            }
            ++p;
        }
        node->text = (text_span){p, 0};
        if (!empty) stack[depth++] = index;
    }
    if (depth || !xml->count || !local_name(xml->node[0].name, "Envelope")) return -1;
    int body = -1;
    for (unsigned i = 1; i < xml->count; ++i) if (xml->node[i].parent == 0) {
        if (local_name(xml->node[i].name, "Header")) { if (xml->header >= 0) return -1; xml->header = (int)i; }
        else if (local_name(xml->node[i].name, "Body")) { if (body >= 0) return -1; body = (int)i; }
        else return -1;
    }
    if (xml->header < 0 || body < 0) return -1;
    for (unsigned i = 1; i < xml->count; ++i) if (xml->node[i].parent == body) {
        if (xml->action >= 0) return -1;
        xml->action = (int)i;
    }
    return xml->action < 0 ? -1 : 0;
}
static int xml_escape(const char *text, char *out, size_t capacity)
{
    size_t used = 0;
    for (; *text; ++text) {
        const char *replace = NULL;
        if (*text == '&') replace = "&amp;"; else if (*text == '<') replace = "&lt;";
        else if (*text == '>') replace = "&gt;"; else if (*text == '"') replace = "&quot;";
        else if (*text == '\'') replace = "&apos;";
        size_t count = replace ? strlen(replace) : 1;
        if (used + count + 1 > capacity) return -1;
        if (replace) memcpy(out + used, replace, count); else out[used] = *text;
        used += count;
    }
    out[used] = 0; return 0;
}
static int make_uuid(char out[37])
{
    unsigned char bytes[16]; if (jipc_random(bytes, sizeof(bytes)) < 0) return -1;
    bytes[6] = (bytes[6] & 15) | 0x40; bytes[8] = (bytes[8] & 63) | 0x80;
    int n = snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return n == 36 ? 0 : -1;
}
static int valid_uuid(const char *text)
{
    if (strlen(text) != 36) return 0;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return 0; }
        else if (!isxdigit((unsigned char)text[i])) return 0;
    }
    return 1;
}
static int read_private(const char *path, char *out, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st; int rc = -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) || st.st_size <= 0 ||
        (uint64_t)st.st_size >= capacity) goto done;
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, out + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto done;
        got += (size_t)n;
    }
    if (memchr(out, 0, got)) goto done;
    out[got] = 0;
    while (got && (out[got - 1] == '\n' || out[got - 1] == '\r')) out[--got] = 0;
    rc = 0;
done:
    close(fd); return rc;
}
static int device_identity(discovery_state *state)
{
    char id_path[640], instance_path[640], lock_path[640], text[96];
    if (snprintf(id_path, sizeof(id_path), "%s/device-id", state->config.state_dir) >= (int)sizeof(id_path) ||
        snprintf(instance_path, sizeof(instance_path), "%s/discovery-instance", state->config.state_dir) >= (int)sizeof(instance_path) ||
        snprintf(lock_path, sizeof(lock_path), "%s/device-id.lock", state->config.state_dir) >= (int)sizeof(lock_path)) return -1;
    if (jipc_mkdirs(state->config.state_dir, 0700) < 0) return -1;
    int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) return -1;
    struct stat st;
    if (fstat(lock, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 077) || flock(lock, LOCK_EX)) { close(lock); return -1; }
    int rc = -1, exists = read_private(id_path, text, sizeof(text));
    if (exists < 0) goto done;
    if (!exists) {
        if (!valid_uuid(text)) goto done;
        strcpy(state->uuid, text);
    } else {
        if (make_uuid(state->uuid) < 0) goto done;
        int n = snprintf(text, sizeof(text), "%s\n", state->uuid);
        if (jipc_atomic_file(id_path, text, (size_t)n, 0600) < 0) goto done;
    }
    exists = read_private(instance_path, text, sizeof(text));
    if (exists < 0) goto done;
    uint32_t previous = 0;
    if (!exists) {
        char *end; errno = 0; unsigned long value = strtoul(text, &end, 10);
        if (!*text || errno || *end || value > UINT32_MAX) goto done;
        previous = (uint32_t)value;
    }
    time_t now = time(NULL);
    uint64_t value = now > 0 ? (uint64_t)now : 1;
    if (value <= previous) value = (uint64_t)previous + 1;
    if (value > UINT32_MAX) goto done;
    state->instance = (uint32_t)value;
    int n = snprintf(text, sizeof(text), "%u\n", state->instance);
    if (jipc_atomic_file(instance_path, text, (size_t)n, 0600) < 0 || make_uuid(state->sequence) < 0) goto done;
    rc = 0;
done:
    flock(lock, LOCK_UN); close(lock); return rc;
}
/* HTTP binds the first usable resolved address. An IPv6-only HTTP endpoint
 * cannot truthfully advertise an IPv4 XAddr, so discovery stays disabled there. */
static int resolve_bind(discovery_state *state)
{
    struct addrinfo hints = {0}, *addresses = NULL;
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(state->config.bind, NULL, &hints, &addresses)) return -1;
    int rc = 1;
    if (addresses && addresses->ai_family == AF_INET) {
        state->bound = ((struct sockaddr_in *)addresses->ai_addr)->sin_addr;
        uint32_t value = ntohl(state->bound.s_addr);
        state->wildcard = value == INADDR_ANY;
        state->loopback = (value & 0xff000000u) == 0x7f000000u;
        rc = 0;
    }
    freeaddrinfo(addresses); return rc;
}
static int peer_local(discovery_state *state, const struct sockaddr_in *peer)
{
    uint32_t value = ntohl(peer->sin_addr.s_addr);
    if (peer->sin_family != AF_INET || ntohs(peer->sin_port) < 1024 || !value ||
        (value & 0xf0000000u) == 0xe0000000u || (value & 0xf0000000u) == 0xf0000000u) return 0;
    if (state->loopback) return (value & 0xff000000u) == 0x7f000000u;
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces)) return 0;
    int allowed = 0;
    for (struct ifaddrs *a = interfaces; a; a = a->ifa_next) {
        if (!a->ifa_addr || !a->ifa_netmask || a->ifa_addr->sa_family != AF_INET ||
            !(a->ifa_flags & IFF_UP)) continue;
        struct in_addr address = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
        struct in_addr mask = ((struct sockaddr_in *)a->ifa_netmask)->sin_addr;
        if (!mask.s_addr || (!state->wildcard && address.s_addr != state->bound.s_addr)) continue;
        uint32_t network = address.s_addr & mask.s_addr;
        if ((peer->sin_addr.s_addr & mask.s_addr) != network) continue;
        uint32_t broadcast = network | ~mask.s_addr;
        if (peer->sin_addr.s_addr == broadcast) continue;
        allowed = 1; break;
    }
    freeifaddrs(interfaces); return allowed;
}
static int reachable_address(discovery_state *state, const struct sockaddr_in *peer, char out[INET_ADDRSTRLEN])
{
    struct in_addr address = state->bound;
    if (state->wildcard) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return -1;
        struct sockaddr_in local = {0}; socklen_t size = sizeof(local);
        int rc = connect(fd, (const struct sockaddr *)peer, sizeof(*peer)) ||
                 getsockname(fd, (struct sockaddr *)&local, &size);
        close(fd); if (rc || size != sizeof(local) || local.sin_family != AF_INET) return -1;
        address = local.sin_addr;
    }
    if (address.s_addr == htonl(INADDR_ANY) || !inet_ntop(AF_INET, &address, out, INET_ADDRSTRLEN)) return -1;
    return 0;
}
static void join_multicast(discovery_state *state)
{
    if (state->loopback) return;
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces)) return;
    for (struct ifaddrs *a = interfaces; a && state->memberships < DISCOVERY_INTERFACES; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !(a->ifa_flags & IFF_UP) ||
            !(a->ifa_flags & IFF_MULTICAST) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        struct in_addr address = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
        if (!state->wildcard && address.s_addr != state->bound.s_addr) continue;
        int duplicate = 0;
        for (unsigned i = 0; i < state->memberships; ++i)
            if (state->membership[i].imr_interface.s_addr == address.s_addr) duplicate = 1;
        if (duplicate) continue;
        struct ip_mreq membership = {0};
        inet_pton(AF_INET, DISCOVERY_GROUP, &membership.imr_multiaddr);
        membership.imr_interface = address;
        if (!setsockopt(state->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)))
            state->membership[state->memberships++] = membership;
    }
    freeifaddrs(interfaces);
}
static int type_match(const char *types)
{
    while (*types) {
        while (whitespace((unsigned char)*types)) ++types;
        if (!*types) break;
        const char *end = types; while (*end && !whitespace((unsigned char)*end)) ++end;
        text_span token = {types, (size_t)(end - types)};
        if (!local_name(token, "NetworkVideoTransmitter") && !local_name(token, "Device")) return 0;
        types = end;
    }
    return 1;
}
static int scope_match(const char *requested)
{
    while (*requested) {
        while (whitespace((unsigned char)*requested)) ++requested;
        if (!*requested) break;
        const char *end = requested; while (*end && !whitespace((unsigned char)*end)) ++end;
        size_t n = (size_t)(end - requested); int found = 0;
        for (unsigned i = 0; i < sizeof(scopes)/sizeof(scopes[0]); ++i)
            if (strlen(scopes[i]) >= n && !memcmp(scopes[i], requested, n) &&
                (!scopes[i][n] || scopes[i][n] == '/')) found = 1;
        if (!found) return 0;
        requested = end;
    }
    return 1;
}
static int allowed_output(discovery_state *state, struct in_addr peer, uint64_t now)
{
    if (now - state->output_window >= 1000000) { state->output_window = now; state->output_count = 0; }
    if (state->output_count >= 8) return 0;
    unsigned oldest = 0;
    for (unsigned i = 0; i < DISCOVERY_PEERS; ++i) {
        if (state->peer[i].address.s_addr == peer.s_addr) {
            if (now < state->peer[i].next) return 0;
            state->peer[i].next = now + 250000; ++state->output_count; return 1;
        }
        if (state->peer[i].next < state->peer[oldest].next) oldest = i;
    }
    state->peer[oldest] = (peer_limit){peer, now + 250000}; ++state->output_count; return 1;
}
static int response_packet(discovery_state *state, const char *packet, size_t length,
                           const struct sockaddr_in *peer, char *out, size_t capacity)
{
    discovery_xml xml;
    if (parse_xml(packet, length, &xml) < 0) return -1;
    int probe = local_name(xml.node[xml.action].name, "Probe");
    int resolve = local_name(xml.node[xml.action].name, "Resolve");
    if (!probe && !resolve) return -1;
    char action[256], id[256], relation[1536];
    if (xml_value(&xml, xml.header, "Action", action, sizeof(action)) < 0 ||
        xml_value(&xml, xml.header, "MessageID", id, sizeof(id)) < 0 || !*id ||
        xml_escape(id, relation, sizeof(relation)) < 0) return -1;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (*p <= 32 || *p >= 127) return -1;
    const char *discovery;
    if (!strcmp(action, DISCOVERY_2005 "/Probe") || !strcmp(action, DISCOVERY_2005 "/Resolve")) discovery = DISCOVERY_2005;
    else if (!strcmp(action, DISCOVERY_2009 "/Probe") || !strcmp(action, DISCOVERY_2009 "/Resolve")) discovery = DISCOVERY_2009;
    else return -1;
    if ((probe && strcmp(strrchr(action, '/') + 1, "Probe")) ||
        (resolve && strcmp(strrchr(action, '/') + 1, "Resolve"))) return -1;
    const char *addressing = strstr(packet, ADDRESSING_2005) ? ADDRESSING_2005 : ADDRESSING_2004;
    const char *anonymous = !strcmp(addressing, ADDRESSING_2005) ? ADDRESSING_2005 "/anonymous" : ADDRESSING_2004 "/role/anonymous";
    int reply = xml_child(&xml, xml.header, "ReplyTo");
    if (reply == -2) return -1;
    if (reply >= 0) {
        char address[256];
        if (xml_value(&xml, reply, "Address", address, sizeof(address)) < 0 || strcmp(address, anonymous)) return -1;
    }
    if (probe) {
        int types_node = xml_child(&xml, xml.action, "Types");
        int scopes_node = xml_child(&xml, xml.action, "Scopes");
        char types[512], requested[1024];
        if (types_node == -2 || scopes_node == -2) return -1;
        if (types_node >= 0 && (xml.node[types_node].has_children ||
            span_copy(xml.node[types_node].text, types, sizeof(types)) < 0 || !type_match(types))) return -1;
        if (scopes_node >= 0) {
            char match[256];
            if (xml.node[scopes_node].match_by.data &&
                (span_copy(xml.node[scopes_node].match_by, match, sizeof(match)) < 0 ||
                 (strcmp(match, DISCOVERY_2005 "/rfc3986") && strcmp(match, DISCOVERY_2009 "/rfc3986")))) return -1;
            if (xml.node[scopes_node].has_children ||
                span_copy(xml.node[scopes_node].text, requested, sizeof(requested)) < 0 || !scope_match(requested)) return -1;
        }
    } else {
        int endpoint = xml_child(&xml, xml.action, "EndpointReference"); char address[128], expected[64];
        snprintf(expected, sizeof(expected), "urn:uuid:%s", state->uuid);
        if (endpoint < 0 || xml_value(&xml, endpoint, "Address", address, sizeof(address)) < 0 || strcmp(address, expected)) return -1;
    }
    uint64_t now = jipc_now_us();
    if (!allowed_output(state, peer->sin_addr, now)) return -1;
    char address[INET_ADDRSTRLEN], message_id[37];
    if (reachable_address(state, peer, address) < 0 || make_uuid(message_id) < 0) return -1;
    if (state->message_number == UINT32_MAX) { if (make_uuid(state->sequence) < 0) return -1; state->message_number = 0; }
    ++state->message_number;
    const char *matches = probe ? "ProbeMatches" : "ResolveMatches";
    const char *match = probe ? "ProbeMatch" : "ResolveMatch";
    int n = snprintf(out, capacity,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:a=\"%s\" xmlns:d=\"%s\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">"
        "<s:Header><a:MessageID>urn:uuid:%s</a:MessageID><a:RelatesTo>%s</a:RelatesTo>"
        "<a:To>%s</a:To><a:Action>%s/%s</a:Action>"
        "<d:AppSequence InstanceId=\"%u\" SequenceId=\"urn:uuid:%s\" MessageNumber=\"%u\"/></s:Header>"
        "<s:Body><d:%s><d:%s><a:EndpointReference><a:Address>urn:uuid:%s</a:Address></a:EndpointReference>"
        "<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types><d:Scopes>%s %s %s %s</d:Scopes>"
        "<d:XAddrs>%s://%s:%u/onvif/device_service</d:XAddrs><d:MetadataVersion>%u</d:MetadataVersion>"
        "</d:%s></d:%s></s:Body></s:Envelope>",
        addressing, discovery, message_id, relation, anonymous, discovery, matches,
        state->instance, state->sequence, state->message_number, matches, match, state->uuid,
        scopes[0], scopes[1], scopes[2], scopes[3], state->config.tls_cert[0] ? "https" : "http",
        address, state->config.http_port, state->instance, match, matches);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}
static void *discovery_main(void *opaque)
{
    discovery_state *state = opaque;
    char packet[DISCOVERY_PACKET + 1], reply[DISCOVERY_PACKET];
    while (!atomic_load(&state->quitting) && !state->app->stop) {
        struct pollfd descriptors[2] = {{state->fd, POLLIN, 0}, {state->wake[0], POLLIN, 0}};
        int ret = poll(descriptors, 2, 250);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0 || descriptors[1].revents) break;
        if (!ret || !(descriptors[0].revents & POLLIN)) continue;
        struct sockaddr_in peer = {0}; socklen_t peer_size = sizeof(peer);
        ssize_t size = recvfrom(state->fd, packet, DISCOVERY_PACKET, MSG_TRUNC,
                                (struct sockaddr *)&peer, &peer_size);
        if (size <= 0 || size > DISCOVERY_PACKET || peer_size != sizeof(peer)) continue;
        uint64_t now = jipc_now_us();
        if (now - state->input_window >= 1000000) { state->input_window = now; state->input_count = 0; }
        if (++state->input_count > 32 || !peer_local(state, &peer)) continue;
        packet[size] = 0;
        int bytes = response_packet(state, packet, (size_t)size, &peer, reply, sizeof(reply));
        if (bytes > 0) (void)sendto(state->fd, reply, (size_t)bytes, MSG_NOSIGNAL,
                                   (struct sockaddr *)&peer, sizeof(peer));
    }
    return NULL;
}
static void discovery_release(discovery_state *state)
{
    if (state->fd >= 0) {
        for (unsigned i = 0; i < state->memberships; ++i)
            setsockopt(state->fd, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                       &state->membership[i], sizeof(state->membership[i]));
        close(state->fd);
    }
    if (state->wake[0] >= 0) close(state->wake[0]);
    if (state->wake[1] >= 0) close(state->wake[1]);
    free(state);
}
int jipc_discovery_start(jipc_app *app)
{
    if (app->discovery_started || app->discovery_state) return -1;
    jipc_config config; jipc_config_copy(app, &config);
    if (!config.discovery) return 0;
    if (!config.discovery_port || config.discovery_port > 65535) return -1;
    discovery_state *state = calloc(1, sizeof(*state)); if (!state) return -1;
    state->app = app; state->config = config; state->fd = state->wake[0] = state->wake[1] = -1;
    atomic_init(&state->quitting, 0);
    int bound = resolve_bind(state);
    if (bound != 0) {
        if (bound > 0) jipc_log("info", "IPv4 discovery disabled for an IPv6-only HTTP bind");
        discovery_release(state); return bound > 0 ? 0 : -1;
    }
    if (device_identity(state) < 0) { jipc_log("error", "Discovery requires private writable device-id state"); discovery_release(state); return -1; }
    state->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (state->fd < 0 || pipe(state->wake)) { discovery_release(state); return -1; }
    int yes = 1;
    setsockopt(state->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    unsigned char ttl = 1, loop = 0;
    setsockopt(state->fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(state->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    int descriptors[] = {state->fd, state->wake[0], state->wake[1]};
    for (unsigned i = 0; i < 3; ++i) {
        int flags = fcntl(descriptors[i], F_GETFL, 0);
        if (flags < 0 || fcntl(descriptors[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(descriptors[i], F_SETFD, FD_CLOEXEC) < 0) { discovery_release(state); return -1; }
    }
    struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)config.discovery_port);
    address.sin_addr.s_addr = state->loopback ? state->bound.s_addr : htonl(INADDR_ANY);
    if (bind(state->fd, (struct sockaddr *)&address, sizeof(address))) { discovery_release(state); return -1; }
    join_multicast(state);
    if (pthread_create(&app->discovery_thread, NULL, discovery_main, state)) { discovery_release(state); return -1; }
    app->discovery_state = state; app->discovery_started = 1;
    jipc_log("info", "WS-Discovery UDP port %u (%u multicast memberships)", config.discovery_port, state->memberships);
    return 0;
}
void jipc_discovery_stop(jipc_app *app)
{
    discovery_state *state = app->discovery_state; if (!state) return;
    atomic_store(&state->quitting, 1);
    unsigned char byte = 1; ssize_t ignored = write(state->wake[1], &byte, 1); (void)ignored;
    if (app->discovery_started) pthread_join(app->discovery_thread, NULL);
    app->discovery_state = NULL; app->discovery_started = 0;
    discovery_release(state);
}
