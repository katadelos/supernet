#include "runtime/runtime.h"
#include "supernet.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = 0;
    }
    return text;
}

static int number(const char *text, unsigned min, unsigned max, unsigned *out)
{
    char *end;
    if (!isdigit((unsigned char)*text)) {
        return -1;
    }
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || *end || value < min || value > max) {
        return -1;
    }
    *out = (unsigned)value;
    return 0;
}

bool sn_config_network_valid(const struct config *cfg)
{
    if (!*cfg->interface || cfg->prefix < 1 || cfg->prefix > 30) {
        return false;
    }
    for (const char *p = cfg->interface; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
            return false;
        }
    }
    struct in_addr ip;
    if (inet_pton(AF_INET, cfg->address, &ip) != 1) {
        return false;
    }
    uint32_t host = ntohl(ip.s_addr);
    uint32_t hostmask = UINT32_MAX >> cfg->prefix;
    return (host >> 24) != 0 && (host >> 24) != 127 && (host >> 24) < 224 &&
           (host & hostmask) != 0 && (host & hostmask) != hostmask;
}

int sn_config(const char *path, struct config *cfg)
{
    *cfg = (struct config){.interface = "usb0",
                           .address = "192.168.15.244",
                           .prefix = 24,
                           .port = 22,
                           .timeout = 20};
    FILE *file = fopen(path, "r");
    if (!file) {
        sn_log(LOG_ERR, "cannot read %s: %s", path, strerror(errno));
        return -1;
    }
    char line[512];
    unsigned lineno = 0, seen = 0;
    int rc = -1;
    while (fgets(line, sizeof(line), file)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(file)) {
            goto invalid;
        }
        char *key = trim(line);
        if (!*key || *key == '#') {
            continue;
        }
        char *value = strchr(key, '=');
        if (!value) {
            goto invalid;
        }
        *value++ = 0;
        key = trim(key);
        value = trim(value);
        unsigned bit;
        if (!strcmp(key, "interface")) {
            bit = 1;
            if (!*value || strlen(value) >= IFNAMSIZ) {
                goto invalid;
            }
            strcpy(cfg->interface, value);
        } else if (!strcmp(key, "address")) {
            bit = 2;
            if (strlen(value) >= sizeof(cfg->address)) {
                goto invalid;
            }
            strcpy(cfg->address, value);
        } else if (!strcmp(key, "prefix")) {
            bit = 4;
            if (number(value, 1, 30, &cfg->prefix)) {
                goto invalid;
            }
        } else if (!strcmp(key, "port")) {
            bit = 8;
            if (number(value, 1, 65535, &cfg->port)) {
                goto invalid;
            }
        } else if (!strcmp(key, "timeout")) {
            bit = 16;
            if (number(value, 5, 60, &cfg->timeout)) {
                goto invalid;
            }
        } else {
            goto invalid;
        }
        if (seen & bit) {
            goto invalid;
        }
        seen |= bit;
    }
    if (ferror(file)) {
        goto invalid;
    }
    if (!sn_config_network_valid(cfg)) {
        goto invalid;
    }
    rc = 0;
    goto done;
invalid:
    sn_log(LOG_ERR,
           "%s:%u: invalid configuration (unknown/duplicate key or invalid value)",
           path,
           lineno);
done:
    fclose(file);
    return rc;
}
