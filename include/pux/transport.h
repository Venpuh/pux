#ifndef PUX_TRANSPORT_H
#define PUX_TRANSPORT_H

#include <stddef.h>

#define PUX_CURL "curl"

int pux_transport_download(const char *url, const char *output_path,
                           size_t max_size, long *http_status,
                           char *error, size_t error_size);

#endif
