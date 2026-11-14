#ifndef HOMEHUB_CLOUD_HTTP_H
#define HOMEHUB_CLOUD_HTTP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {const char *key,*value;} HH_CloudHeader;
typedef struct {
    const char *method,*host,*path,*ca_pem;
    const HH_CloudHeader *headers;size_t header_count;
    const uint8_t *body;size_t body_bytes,max_body;
    uint32_t timeout_ms;
    /* Zero initialization preserves verified HTTPS on 443. Plain HTTP must
     * be selected explicitly and requires ca_pem == NULL. host never includes
     * a scheme or port; port == 0 selects HTTPS 443 / plain HTTP 80. */
    bool plain_http;uint16_t port;
} HH_CloudRequest;
typedef struct {
    uint8_t *storage;const uint8_t *body;size_t body_bytes;int status;
} HH_CloudResponse;
/* A background request must yield to foreground audio; no request retains an
 * app/audio/filesystem mutex. Idle is updated by the application task. */
void hh_cloud_http_background_idle(bool idle);
bool hh_cloud_http_request(const HH_CloudRequest *request,HH_CloudResponse *response);
void hh_cloud_http_free(HH_CloudResponse *response);
#endif
