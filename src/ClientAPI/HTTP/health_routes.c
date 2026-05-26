//
// Created by victor on 5/26/25.
//
#include "health_routes.h"
#include "http_request.h"
#include "http_response.h"
#include <string.h>
#include <http_parser.h>

static int _health_middleware(http_request_t* request, http_response_t* response,
                              void* user_data) {
  if (request->method != HTTP_GET || strcmp(request->path, "/health") != 0) {
    return 0;
  }

  const health_context_t* ctx = (const health_context_t*)user_data;
  health_data_t data = health_data_collect(ctx);

  char json[8192];
  size_t len = health_data_to_json(&data, json, sizeof(json));
  if (len == 0) {
    http_response_set_status(response, HTTP_STATUS_INTERNAL_SERVER_ERROR);
    http_response_end(response);
    return 1;
  }

  http_response_set_status(response, HTTP_STATUS_OK);
  http_response_set_header(response, "Content-Type", "application/json");
  http_response_write(response, json, len);
  http_response_end(response);
  return 1;
}

void health_routes_register(http_server_t* server, const health_context_t* ctx) {
  http_server_use(server, _health_middleware, (void*)ctx, NULL);
}
