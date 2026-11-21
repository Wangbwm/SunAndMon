#include "clipboard_api.h"

#include "src/clipboard_http.h"

namespace nvhttp::clipboard_api {

  void
  upload_blob(resp_https_t response, req_https_t request) {
    auto out = clipboard_http::process_blob_upload(request);
    clipboard_http::write_blob_response(response, out);
  }

  void
  init_blob(resp_https_t response, req_https_t request) {
    auto out = clipboard_http::process_blob_init(request);
    clipboard_http::write_blob_response(response, out);
  }

  void
  upload_blob_chunk(resp_https_t response, req_https_t request) {
    auto out = clipboard_http::process_blob_chunk(request);
    clipboard_http::write_blob_response(response, out);
  }

  void
  get_blob(resp_https_t response, req_https_t request) {
    auto out = clipboard_http::process_blob_get(request);
    clipboard_http::write_blob_response(response, out);
  }

}  // namespace nvhttp::clipboard_api
