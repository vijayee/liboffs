#include "file-stream.h"
#include "../Util/allocator.h"
#include "../Workers/error.h"
#include "../Buffer/buffer.h"
#include <fcntl.h>
#include <unistd.h>


readable_file_stream_t* readable_file_stream_create(char* filename, size_t chunk_size, int* error_code) {
  readable_file_stream_t* rs= get_clear_memory(sizeof(readable_file_stream_t));
  stream_init(rs, push, readable_stream,
              (void(*)(stream_t*))readable_file_stream_destroy);
  rs->filename = filename;
  rs->chunk_size = chunk_size;
  readable_stream_push_handler((stream_t*) rs, (void (*)(stream_t*))readable_file_stream_push);

#ifdef _WIN32
  rs->fd = open(rs->filename, _O_RDONLY | _O_BINARY | _O_CREAT, 0644);
#else
  rs->fd = open(rs->filename, O_RDONLY | O_CREAT, 0644);
#endif
  int file_size = lseek(rs->fd, 0, SEEK_END);
  if (lseek(rs->fd, 0, SEEK_SET) < 0) {
    *error_code = -1;
  }
  return rs;
}
void readable_file_stream_destyoy(readable_file_stream_t* stream) {

}

void readable_file_stream_push(readable_file_stream_t* stream) {
  platform_lock(&stream->stream.lock);
  if (stream->stream.is_destroyed == 1) {
    platform_unlock(&stream->stream.lock);
    stream_notify(stream, error_event, ERROR("Stream is already destroyed"));
  } else {
    int32_t diff = stream->file_size - stream->cursor;
    size_t size;
    if (stream->chunk_size > diff) {
      size = diff;
    } else {
      size = stream->chunk_size;
    }
    uint8_t* buf = get_memory(size);
    size_t bytes = read(stream->fd, buf, size);
    if (bytes != size) {
      free(buf);
      platform_unlock(&stream->stream.lock);
      stream_notify(stream, error_event, ERROR("Invalid Read Size"));
      return;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

    stream->cursor += size;
    platform_unlock(&stream->stream.lock);
    stream_notify(stream, data_event, CONSUME(buffer, buffer_t));
  }
}

void readable_file_stream_read(readable_file_stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*)) {
  platform_lock(&stream->stream.lock);
  if (stream->stream.is_destroyed == 1) {
    platform_unlock(&stream->stream.lock);
    stream_notify(stream, error_event, ERROR("Stream is already destroyed"));
  } else {
    int32_t diff = stream->file_size - stream->cursor;
    if (size < diff) {
      size = diff;
    }
    uint8_t* buf = get_memory(size);
    size_t bytes = read(stream->fd, buf, size);
    if (bytes != size) {
      free(buf);
      platform_unlock(&stream->stream.lock);
      stream_notify(stream, error_event, ERROR("Invalid Read Size"));
      return;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

    stream->cursor += size;
    platform_unlock(&stream->stream.lock);
    cb(ctx, CONSUME(buffer, buffer_t));
  }
}

writeable_file_stream_t* writeable_file_stream_create(char* filename) {
  writeable_file_stream_t* ws = get_clear_memory(sizeof(writeable_file_stream_t));
  stream_init((stream_t*) ws, push, writeable_stream);
  writeable_stream_write_handler((stream_t*) ws, (void (*)(stream_t*, void*)) writeable_file_stream_write);

#ifdef _WIN32
  ws->fd = open(ws->filename, _O_WRONLY | _O_BINARY | _O_CREAT, 0644);
#else
  ws->fd = open(ws->filename, O_WRONLY | O_CREAT, 0644);
#endif
  return ws;
}

void writeable_file_stream_write(writeable_file_stream_t* stream, buffer_t* data) {
  write(stream->fd, data->data, data->size);
}