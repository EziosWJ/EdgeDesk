/*
 * EdgeDesk native backend example (SPEC-0011).
 *
 * A minimal POSIX HTTP/1.1 service over a Unix domain socket. It uses libc
 * only: no shell, no scripting runtime and no third-party library.
 *
 * Environment (set by EdgeDesk, never by the application manifest):
 *   EDGEDESK_APP_SOCKET    absolute Unix socket path the host will connect to
 *   EDGEDESK_APP_DATA_DIR  absolute persistent data directory owned by the app
 *
 * Routes:
 *   GET  /health  -> {"ok":true,"app":"example"}
 *   GET  /notes   -> {"notes":["...", ...]}
 *   POST /notes   -> {"ok":true}           body: {"text":"..."}
 *
 * Notes are stored one JSON-escaped string per line in <data>/notes.txt.
 * Every request closes its connection; the host proxy sends one request per
 * connection. The service is deliberately single-threaded and bounded.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_HEADER_BYTES (16 * 1024)
#define MAX_BODY_BYTES (1024 * 1024)
#define MAX_NOTES_BYTES (256 * 1024)
#define MAX_NOTE_BYTES 8192
#define NOTES_FILENAME "notes.txt"
#define SOCKET_MODE 0660

static int g_data_dir = -1;

static void fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("notes-backend: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

static int send_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length > 0) {
        const ssize_t written = send(fd, cursor, length, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) return -1;
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int write_all(int fd, const void *data, size_t length) {
    const unsigned char *cursor = data;
    while (length > 0) {
        const ssize_t written = write(fd, cursor, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) return -1;
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static void respond(int fd, int status, const char *reason, const char *body) {
    char header[256];
    const size_t body_length = strlen(body);
    const int length = snprintf(header, sizeof header,
                                "HTTP/1.1 %d %s\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: %zu\r\n"
                                "Connection: close\r\n"
                                "\r\n",
                                status, reason, body_length);
    if (length < 0 || (size_t)length >= sizeof header) return;
    if (send_all(fd, header, (size_t)length) != 0) return;
    (void)send_all(fd, body, body_length);
}

/* Reads the notes file. A missing file means "no notes yet". */
static int read_notes(char *buffer, size_t capacity, size_t *length) {
    *length = 0;
    const int fd = openat(g_data_dir, NOTES_FILENAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    size_t used = 0;
    for (;;) {
        if (used == capacity) {
            close(fd);
            errno = ENOSPC;
            return -1;
        }
        const ssize_t count = read(fd, buffer + used, capacity - used);
        if (count < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    close(fd);
    *length = used;
    return 0;
}

/* Appends one escaped JSON string plus a newline. Returns -2 when full. */
static int append_note(const char *encoded, size_t encoded_length) {
    const int fd = openat(g_data_dir, NOTES_FILENAME,
                          O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    struct stat info;
    if (fstat(fd, &info) != 0) {
        close(fd);
        return -1;
    }
    if (info.st_size < 0 || (uintmax_t)info.st_size + encoded_length + 1 > MAX_NOTES_BYTES) {
        close(fd);
        errno = ENOSPC;
        return -2;
    }
    int result = 0;
    if (write_all(fd, encoded, encoded_length) != 0) {
        result = -1;
    } else if (write_all(fd, "\n", 1) != 0) {
        result = -1;
    }
    close(fd);
    return result;
}

static size_t skip_space(const char *text, size_t index, size_t length) {
    while (index < length) {
        const char c = text[index];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        index += 1;
    }
    return index;
}

/* Returns the index just past the closing quote, or 0 when malformed. */
static size_t skip_json_string(const char *text, size_t index, size_t length) {
    if (index >= length || text[index] != '"') return 0;
    index += 1;
    while (index < length) {
        const char c = text[index];
        if (c == '\\') {
            if (index + 1 >= length) return 0;
            index += 2;
            continue;
        }
        if (c == '"') return index + 1;
        index += 1;
    }
    return 0;
}

/*
 * Finds a top-level-looking object member and returns the index of the value
 * start. This is intentionally small: the example only accepts a flat
 * {"text":"..."} body.
 */
static size_t find_member(const char *text, size_t length, const char *key) {
    const size_t key_length = strlen(key);
    size_t index = skip_space(text, 0, length);
    if (index < length && text[index] == '{') index += 1;
    for (;;) {
        index = skip_space(text, index, length);
        while (index < length && text[index] == ',') {
            index = skip_space(text, index + 1, length);
        }
        if (index >= length || text[index] != '"') return 0;
        const size_t key_start = index + 1;
        const size_t after_key = skip_json_string(text, index, length);
        if (after_key == 0) return 0;
        const size_t key_end = after_key - 1;
        size_t cursor = skip_space(text, after_key, length);
        if (cursor >= length || text[cursor] != ':') return 0;
        cursor = skip_space(text, cursor + 1, length);
        if (key_end - key_start == key_length &&
            memcmp(text + key_start, key, key_length) == 0) {
            return cursor;
        }
        /* Skip the value conservatively so it cannot be mistaken for a key. */
        if (cursor < length && text[cursor] == '"') {
            const size_t after_value = skip_json_string(text, cursor, length);
            if (after_value == 0) return 0;
            index = after_value;
        } else {
            while (cursor < length && text[cursor] != ',' && text[cursor] != '}') cursor += 1;
            index = cursor;
        }
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int append_utf8(char *output, size_t capacity, size_t *used, uint32_t codepoint) {
    unsigned char bytes[4];
    size_t count = 0;
    if (codepoint < 0x80) {
        bytes[count++] = (unsigned char)codepoint;
    } else if (codepoint < 0x800) {
        bytes[count++] = (unsigned char)(0xc0 | (codepoint >> 6));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
        bytes[count++] = (unsigned char)(0xe0 | (codepoint >> 12));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3f));
    } else {
        bytes[count++] = (unsigned char)(0xf0 | (codepoint >> 18));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[count++] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[count++] = (unsigned char)(0x80 | (codepoint & 0x3f));
    }
    if (*used + count > capacity) return -1;
    memcpy(output + *used, bytes, count);
    *used += count;
    return 0;
}

/*
 * Decodes a JSON string starting at text[index] into output.
 * Returns 0 on success, -1 on malformed input, -2 when output is too small.
 */
static int decode_json_string(const char *text, size_t length, size_t index,
                              char *output, size_t capacity, size_t *out_length) {
    *out_length = 0;
    if (index >= length || text[index] != '"') return -1;
    index += 1;
    while (index < length) {
        const unsigned char c = (unsigned char)text[index];
        if (c == '"') return 0;
        if (c < 0x20) return -1;
        if (c != '\\') {
            if (*out_length + 1 > capacity) return -2;
            output[(*out_length)++] = (char)c;
            index += 1;
            continue;
        }
        index += 1;
        if (index >= length) return -1;
        const char escape = text[index++];
        char plain = 0;
        switch (escape) {
            case '"': plain = '"'; break;
            case '\\': plain = '\\'; break;
            case '/': plain = '/'; break;
            case 'b': plain = '\b'; break;
            case 'f': plain = '\f'; break;
            case 'n': plain = '\n'; break;
            case 'r': plain = '\r'; break;
            case 't': plain = '\t'; break;
            case 'u': {
                if (index + 4 > length) return -1;
                uint32_t codepoint = 0;
                for (int digit = 0; digit < 4; digit += 1) {
                    const int value = hex_value(text[index + (size_t)digit]);
                    if (value < 0) return -1;
                    codepoint = (codepoint << 4) | (uint32_t)value;
                }
                index += 4;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (index + 6 > length || text[index] != '\\' || text[index + 1] != 'u') return -1;
                    uint32_t low = 0;
                    for (int digit = 0; digit < 4; digit += 1) {
                        const int value = hex_value(text[index + 2 + (size_t)digit]);
                        if (value < 0) return -1;
                        low = (low << 4) | (uint32_t)value;
                    }
                    if (low < 0xdc00 || low > 0xdfff) return -1;
                    index += 6;
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return -1;
                }
                if (codepoint == 0) return -1;
                if (append_utf8(output, capacity, out_length, codepoint) != 0) return -2;
                continue;
            }
            default:
                return -1;
        }
        if (*out_length + 1 > capacity) return -2;
        output[(*out_length)++] = plain;
    }
    return -1;
}

/* Encodes text as a JSON string including the surrounding quotes. */
static int encode_json_string(const char *text, size_t length, char *output,
                              size_t capacity, size_t *out_length) {
    size_t used = 0;
    const char *hex = "0123456789abcdef";
    if (used + 1 > capacity) return -1;
    output[used++] = '"';
    for (size_t index = 0; index < length; index += 1) {
        const unsigned char c = (unsigned char)text[index];
        const char *escape = NULL;
        char unicode[7];
        switch (c) {
            case '"': escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
            default:
                if (c < 0x20) {
                    unicode[0] = '\\';
                    unicode[1] = 'u';
                    unicode[2] = '0';
                    unicode[3] = '0';
                    unicode[4] = hex[(c >> 4) & 0x0f];
                    unicode[5] = hex[c & 0x0f];
                    unicode[6] = '\0';
                    escape = unicode;
                }
                break;
        }
        if (escape == NULL) {
            if (used + 1 > capacity) return -1;
            output[used++] = (char)c;
        } else {
            const size_t escape_length = strlen(escape);
            if (used + escape_length > capacity) return -1;
            memcpy(output + used, escape, escape_length);
            used += escape_length;
        }
    }
    if (used + 1 > capacity) return -1;
    output[used++] = '"';
    *out_length = used;
    return 0;
}

/* A line must be exactly one complete, unescaped JSON string. */
static bool valid_stored_line(const char *line, size_t length) {
    if (length < 2 || line[0] != '"' || line[length - 1] != '"') return false;
    bool escaped = false;
    for (size_t index = 1; index + 1 < length; index += 1) {
        const unsigned char c = (unsigned char)line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"' || c < 0x20) return false;
    }
    return !escaped;
}

static void handle_get_notes(int fd) {
    char notes[MAX_NOTES_BYTES];
    size_t length = 0;
    if (read_notes(notes, sizeof notes, &length) != 0) {
        respond(fd, 500, "Internal Server Error", "{\"error\":\"storage_read_failed\"}");
        return;
    }
    const size_t capacity = length + length / 2 + 128;
    char *body = malloc(capacity);
    if (body == NULL) {
        respond(fd, 500, "Internal Server Error", "{\"error\":\"out_of_memory\"}");
        return;
    }
    size_t used = 0;
    memcpy(body, "{\"notes\":[", 10);
    used = 10;
    bool first = true;
    size_t cursor = 0;
    bool corrupted = false;
    while (cursor < length) {
        const size_t start = cursor;
        while (cursor < length && notes[cursor] != '\n') cursor += 1;
        const size_t line_length = cursor - start;
        if (cursor < length) cursor += 1;
        if (line_length == 0) continue;
        if (!valid_stored_line(notes + start, line_length)) {
            corrupted = true;
            break;
        }
        if (!first) body[used++] = ',';
        first = false;
        memcpy(body + used, notes + start, line_length);
        used += line_length;
    }
    if (corrupted) {
        free(body);
        respond(fd, 500, "Internal Server Error", "{\"error\":\"storage_corrupted\"}");
        return;
    }
    memcpy(body + used, "]}", 2);
    used += 2;
    body[used] = '\0';
    respond(fd, 200, "OK", body);
    free(body);
}

static void handle_post_notes(int fd, const char *body, size_t length) {
    size_t cursor = skip_space(body, 0, length);
    if (cursor >= length || body[cursor] != '{') {
        respond(fd, 400, "Bad Request", "{\"error\":\"json_object_required\"}");
        return;
    }
    cursor = find_member(body, length, "text");
    if (cursor == 0) {
        respond(fd, 400, "Bad Request", "{\"error\":\"text_required\"}");
        return;
    }
    char note[MAX_NOTE_BYTES];
    size_t note_length = 0;
    const int decoded = decode_json_string(body, length, cursor, note, sizeof note, &note_length);
    if (decoded == -2) {
        respond(fd, 413, "Payload Too Large", "{\"error\":\"note_too_large\"}");
        return;
    }
    if (decoded != 0) {
        respond(fd, 400, "Bad Request", "{\"error\":\"invalid_text\"}");
        return;
    }
    char encoded[MAX_NOTE_BYTES * 6 + 2];
    size_t encoded_length = 0;
    if (encode_json_string(note, note_length, encoded, sizeof encoded, &encoded_length) != 0) {
        respond(fd, 400, "Bad Request", "{\"error\":\"invalid_text\"}");
        return;
    }
    const int result = append_note(encoded, encoded_length);
    if (result == -2) {
        respond(fd, 507, "Insufficient Storage", "{\"error\":\"notes_full\"}");
        return;
    }
    if (result != 0) {
        respond(fd, 500, "Internal Server Error", "{\"error\":\"storage_write_failed\"}");
        return;
    }
    respond(fd, 201, "Created", "{\"ok\":true}");
}

/* Reads headers and body. Returns an HTTP status code, or 0 on success. */
static int read_request(int fd, char *header, size_t capacity, size_t *header_length,
                        char **body_out, size_t *body_length) {
    size_t used = 0;
    size_t end = 0;
    while (end == 0) {
        if (used == capacity) return 431;
        const ssize_t count = recv(fd, header + used, capacity - used, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            return 400;
        }
        if (count == 0) return 400;
        const size_t previous = used;
        used += (size_t)count;
        const size_t from = previous >= 3 ? previous - 3 : 0;
        for (size_t index = from; index + 4 <= used; index += 1) {
            if (memcmp(header + index, "\r\n\r\n", 4) == 0) {
                end = index + 4;
                break;
            }
        }
    }
    *header_length = end;
    char *head = malloc(end + 1);
    if (head == NULL) return 500;
    memcpy(head, header, end);
    head[end] = '\0';

    size_t content_length = 0;
    bool have_length = false;
    const char *line = strstr(head, "\r\n");
    while (line != NULL) {
        line += 2;
        const char *line_end = strstr(line, "\r\n");
        if (line_end == NULL || line_end == line) break;
        const size_t line_length = (size_t)(line_end - line);
        if (line_length > 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            free(head);
            return 400;
        }
        if (line_length > 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            if (have_length) {
                free(head);
                return 400;
            }
            const char *value = line + 15;
            while (value < line_end && (*value == ' ' || *value == '\t')) value += 1;
            if (value == line_end) {
                free(head);
                return 400;
            }
            char *parse_end = NULL;
            errno = 0;
            const unsigned long long parsed = strtoull(value, &parse_end, 10);
            if (errno != 0 || parse_end != line_end || parsed > MAX_BODY_BYTES) {
                free(head);
                return parsed > MAX_BODY_BYTES ? 413 : 400;
            }
            content_length = (size_t)parsed;
            have_length = true;
        }
        line = line_end;
    }
    free(head);

    const size_t already = used > end ? used - end : 0;
    if (already > content_length) return 400;
    char *body = malloc(content_length + 1);
    if (body == NULL) return 500;
    if (already > 0) memcpy(body, header + end, already);
    size_t filled = already;
    while (filled < content_length) {
        const ssize_t count = recv(fd, body + filled, content_length - filled, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            free(body);
            return 400;
        }
        if (count == 0) {
            free(body);
            return 400;
        }
        filled += (size_t)count;
    }
    body[content_length] = '\0';
    *body_out = body;
    *body_length = content_length;
    return 0;
}

static void handle_client(int fd) {
    struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

    char header[MAX_HEADER_BYTES];
    size_t header_length = 0;
    char *body = NULL;
    size_t body_length = 0;
    const int status = read_request(fd, header, sizeof header, &header_length, &body, &body_length);
    if (status != 0) {
        if (status == 413) respond(fd, 413, "Payload Too Large", "{\"error\":\"request_too_large\"}");
        else if (status == 431) respond(fd, 431, "Request Header Fields Too Large", "{\"error\":\"headers_too_large\"}");
        else if (status == 500) respond(fd, 500, "Internal Server Error", "{\"error\":\"out_of_memory\"}");
        else respond(fd, 400, "Bad Request", "{\"error\":\"malformed_request\"}");
        return;
    }

    char *request = malloc(header_length + 1);
    if (request == NULL) {
        free(body);
        respond(fd, 500, "Internal Server Error", "{\"error\":\"out_of_memory\"}");
        return;
    }
    memcpy(request, header, header_length);
    request[header_length] = '\0';

    char *line_end = strstr(request, "\r\n");
    if (line_end == NULL) {
        free(request);
        free(body);
        respond(fd, 400, "Bad Request", "{\"error\":\"malformed_request\"}");
        return;
    }
    *line_end = '\0';
    char *method = request;
    char *first_space = strchr(method, ' ');
    char *target = NULL;
    if (first_space != NULL) {
        *first_space = '\0';
        target = first_space + 1;
        char *second_space = strchr(target, ' ');
        if (second_space != NULL) *second_space = '\0';
    }
    if (first_space == NULL || target == NULL || target[0] != '/') {
        free(request);
        free(body);
        respond(fd, 400, "Bad Request", "{\"error\":\"malformed_request_line\"}");
        return;
    }
    char *query = strchr(target, '?');
    if (query != NULL) *query = '\0';
    if (strstr(target, "..") != NULL) {
        free(request);
        free(body);
        respond(fd, 400, "Bad Request", "{\"error\":\"invalid_path\"}");
        return;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(target, "/health") == 0) {
            respond(fd, 200, "OK", "{\"ok\":true,\"app\":\"example\"}");
        } else if (strcmp(target, "/notes") == 0) {
            handle_get_notes(fd);
        } else {
            respond(fd, 404, "Not Found", "{\"error\":\"not_found\"}");
        }
    } else if (strcmp(method, "POST") == 0) {
        if (strcmp(target, "/notes") == 0) {
            handle_post_notes(fd, body, body_length);
        } else {
            respond(fd, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
        }
    } else if (strcmp(target, "/health") == 0 || strcmp(target, "/notes") == 0) {
        respond(fd, 405, "Method Not Allowed", "{\"error\":\"method_not_allowed\"}");
    } else {
        respond(fd, 404, "Not Found", "{\"error\":\"not_found\"}");
    }

    free(request);
    free(body);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char *socket_path = getenv("EDGEDESK_APP_SOCKET");
    const char *data_dir = getenv("EDGEDESK_APP_DATA_DIR");
    if (socket_path == NULL || socket_path[0] == '\0') {
        fatal("EDGEDESK_APP_SOCKET is not set; start this application through EdgeDesk");
    }
    if (data_dir == NULL || data_dir[0] == '\0') {
        fatal("EDGEDESK_APP_DATA_DIR is not set; start this application through EdgeDesk");
    }
    if (socket_path[0] != '/') {
        fatal("EDGEDESK_APP_SOCKET must be an absolute path");
    }
    if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fatal("EDGEDESK_APP_SOCKET is too long for an AF_UNIX socket path");
    }
    if (data_dir[0] != '/') {
        fatal("EDGEDESK_APP_DATA_DIR must be an absolute path");
    }

    g_data_dir = open(data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (g_data_dir < 0) {
        fatal("cannot open EDGEDESK_APP_DATA_DIR %s: %s", data_dir, strerror(errno));
    }

    struct stat existing;
    if (lstat(socket_path, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            fatal("refusing to replace non-socket path %s", socket_path);
        }
        if (unlink(socket_path) != 0) {
            fatal("cannot remove stale socket %s: %s", socket_path, strerror(errno));
        }
    } else if (errno != ENOENT) {
        fatal("cannot inspect socket path %s: %s", socket_path, strerror(errno));
    }

    const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) fatal("socket() failed: %s", strerror(errno));

    struct sockaddr_un address;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    const size_t socket_length = strlen(socket_path);
    memcpy(address.sun_path, socket_path, socket_length + 1);
    const socklen_t address_length =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + socket_length + 1);
    if (bind(listener, (struct sockaddr *)&address, address_length) != 0) {
        fatal("cannot bind %s: %s", socket_path, strerror(errno));
    }
    /* The host creates the run directory with a shared group. 0660 lets the
     * EdgeDesk backend connect without exposing the socket to other users. */
    if (chmod(socket_path, SOCKET_MODE) != 0) {
        const int saved = errno;
        close(listener);
        unlink(socket_path);
        fatal("cannot chmod %s: %s", socket_path, strerror(saved));
    }
    if (listen(listener, 16) != 0) {
        const int saved = errno;
        close(listener);
        unlink(socket_path);
        fatal("listen() failed: %s", strerror(saved));
    }

    fprintf(stderr, "notes-backend: listening on %s (data: %s)\n", socket_path, data_dir);

    for (;;) {
        const int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            fprintf(stderr, "notes-backend: accept() failed: %s\n", strerror(errno));
            break;
        }
        handle_client(client);
        close(client);
    }

    close(listener);
    unlink(socket_path);
    close(g_data_dir);
    return 0;
}
