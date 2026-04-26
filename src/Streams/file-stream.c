#include "file-stream.h"
#include "../Util/allocator.h"
#include "../Workers/error.h"
#include "../Buffer/buffer.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

void _readable_pull_file_stream_on_pull(readable_pull_file_stream_t* stream);

readable_push_file_stream_t* readable_push_file_stream_create(priority_t* priority, work_pool_t* pool, char* filename, size_t chunk_size, int* error_code) {
  *error_code = 0;
  readable_push_file_stream_t* rs= get_clear_memory(sizeof(readable_push_file_stream_t));

  rs->filename = strdup(filename);
  rs->chunk_size = chunk_size;
#ifdef _WIN32
  rs->fd = open(rs->filename, _O_RDONLY | _O_BINARY | _O_CREAT, 0644);
#else
  rs->fd = open(rs->filename, O_RDONLY | O_CREAT, 0644);
#endif
  int file_size = lseek(rs->fd, 0, SEEK_END);
  rs->file_size = file_size;
  if (lseek(rs->fd, 0, SEEK_SET) < 0) {
    *error_code = -1;
  }
  stream_init((stream_t*) rs, push, readable_stream, priority, 1, pool,
              (void(*)(stream_t*))readable_push_file_stream_destroy);
  readable_stream_push_handler((stream_t*) rs, (void (*)(stream_t*))readable_push_file_stream_push);
  stream_close_handler((stream_t*) rs,(void(*)(stream_t*))readable_push_file_stream_close);
  return rs;
}
void readable_push_file_stream_destroy(readable_push_file_stream_t* stream) {
  refcounter_dereference((refcounter_t*) stream);
  if (refcounter_count((refcounter_t*) stream) == 0) {
#ifdef _WIN32
    _close(stream->fd);
#else
    close(stream->fd);
#endif
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}

void readable_push_file_stream_push(readable_push_file_stream_t* stream) {
  platform_lock(&stream->stream.lock);
  if (stream->stream.is_deactivated == 1) {
    platform_unlock(&stream->stream.lock);
    stream_notify((stream_t*)stream, error_event, ERROR("Stream is already destroyed"));
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
      stream_notify((stream_t*)stream, error_event, ERROR("Invalid Read Size"));
      return;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

    stream->cursor += size;
    platform_unlock(&stream->stream.lock);
    stream_notify((stream_t*)stream, data_event, CONSUME(buffer, buffer_t));
    if (stream->file_size == stream->cursor) {
      stream_notify((stream_t*) stream, complete_event, NULL);
      stream_close((stream_t*) stream);
    } else {
      readable_push_stream_push((stream_t*) stream);
    }
  }
}

void readable_push_file_stream_read(readable_push_file_stream_t* stream, size_t size, void* ctx, void (*cb)(void*, void*)) {
  platform_lock(&stream->stream.lock);
  if (stream->stream.is_deactivated == 1) {
    platform_unlock(&stream->stream.lock);
    stream_notify((stream_t*)stream, error_event, ERROR("Stream is already destroyed"));
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
      stream_notify((stream_t*)stream, error_event, ERROR("Invalid Read Size"));
      return;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

    stream->cursor += size;
    platform_unlock(&stream->stream.lock);
    cb(ctx, CONSUME(buffer, buffer_t));
  }
}

void readable_push_file_stream_close(readable_push_file_stream_t* stream) {
  platform_lock(&stream->stream.lock);
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
  }
  platform_unlock(&stream->stream.lock);
  if (deactivated == 0) {
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t*) stream, close_event, NULL);
  }
}

writeable_push_file_stream_t* writeable_push_file_stream_create(priority_t* priority, work_pool_t* pool, char* filename) {
  writeable_push_file_stream_t* ws = get_clear_memory(sizeof(writeable_push_file_stream_t));
  stream_init((stream_t*) ws, push, writeable_stream, priority, 0, pool, (void(*)(stream_t*)) writeable_push_file_stream_destroy);
  writeable_stream_write_handler((stream_t*) ws, (void (*)(stream_t*, void*)) writeable_push_file_stream_write);
  stream_close_handler((stream_t*) ws, (void(*)(stream_t*))writeable_push_file_stream_close);
  ws->filename = strdup(filename);
#ifdef _WIN32
  ws->fd = open(ws->filename, _O_WRONLY | _O_BINARY | _O_CREAT, 0644);
#else
  ws->fd = open(ws->filename, O_WRONLY | O_CREAT, 0644);
#endif
  return ws;
}

void writeable_push_file_stream_write(writeable_push_file_stream_t* stream, buffer_t* data) {
  REFERENCE(data, buffer_t);
  write(stream->fd, data->data, data->size);
  DESTROY(data, buffer);
}
void writeable_push_file_stream_destroy(writeable_push_file_stream_t* stream) {
  refcounter_dereference((refcounter_t*) stream);
  if (refcounter_count((refcounter_t*) stream) == 0) {
#ifdef _WIN32
    _close(stream->fd);
#else
    close(stream->fd);
#endif
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}
void writeable_push_file_stream_close(writeable_push_file_stream_t* stream) {
  platform_lock(&stream->stream.lock);
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
    platform_unlock(&stream->stream.lock);
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t *) stream, close_event, NULL);
  } else {
    platform_unlock(&stream->stream.lock);
  }
}

readable_pull_file_stream_t* readable_pull_file_stream_create(priority_t* priority, work_pool_t* pool, char* filename, size_t chunk_size, int* error_code) {
  *error_code = 0;
  readable_pull_file_stream_t* rs= get_clear_memory(sizeof(readable_pull_file_stream_t));

  rs->filename = strdup(filename);
  rs->chunk_size = chunk_size;
#ifdef _WIN32
  rs->fd = open(rs->filename, _O_RDONLY | _O_BINARY | _O_CREAT, 0644);
#else
  rs->fd = open(rs->filename, O_RDONLY | O_CREAT, 0644);
#endif
  int file_size = lseek(rs->fd, 0, SEEK_END);
  rs->file_size = file_size;
  if (lseek(rs->fd, 0, SEEK_SET) < 0) {
    *error_code = -1;
  }
  stream_init((stream_t*) rs, pull, readable_stream, priority, 0, pool,
              (void(*)(stream_t*))readable_pull_file_stream_destroy);
  stream_close_handler((stream_t*) rs,(void(*)(stream_t*))readable_pull_file_stream_close);
  readable_stream_pull_handler((stream_t*)rs,(void(*)(stream_t*)) _readable_pull_file_stream_on_pull);
  return rs;
}

void readable_pull_file_stream_destroy(readable_pull_file_stream_t* stream) {
  refcounter_dereference((refcounter_t*) stream);
  if (refcounter_count((refcounter_t*) stream) == 0) {
#ifdef _WIN32
    _close(stream->fd);
#else
    close(stream->fd);
#endif
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}

void _readable_pull_file_stream_on_pull(readable_pull_file_stream_t* stream) {
  platform_lock(&stream->stream.lock);
  if (stream->stream.is_deactivated == 1) {
    platform_unlock(&stream->stream.lock);
    stream_notify((stream_t*)stream, error_event, ERROR("Stream is already destroyed"));
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
      stream_notify((stream_t*)stream, error_event, ERROR("Invalid Read Size"));
      return;
    }

    buffer_t* buffer = buffer_create_from_existing_memory(buf, size);

    stream->cursor += size;
    platform_unlock(&stream->stream.lock);
    stream_notify((stream_t*)stream, data_event, CONSUME(buffer, buffer_t));
    if (stream->file_size == stream->cursor) {
      stream_notify((stream_t*) stream, complete_event, NULL);
      stream_close((stream_t*) stream);
    }
  }
}

void readable_pull_file_stream_close(readable_pull_file_stream_t* stream) {
  platform_lock(&stream->stream.lock);
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
  }
  platform_unlock(&stream->stream.lock);
  if (deactivated == 0) {
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t*) stream, close_event, NULL);
  }
}

writeable_pull_file_stream_t* writeable_pull_file_stream_create(priority_t* priority, work_pool_t* pool, char* filename) {
  writeable_pull_file_stream_t* ws = get_clear_memory(sizeof(writeable_pull_file_stream_t));
  stream_init((stream_t*) ws, pull, writeable_stream, priority, 0, pool, (void(*)(stream_t*)) writeable_pull_file_stream_destroy);
  writeable_stream_write_handler((stream_t*) ws, (void (*)(stream_t*, void*)) writeable_pull_file_stream_write);
  stream_close_handler((stream_t*) ws, (void(*)(stream_t*))writeable_pull_file_stream_close);
  ws->filename = strdup(filename);
#ifdef _WIN32
  ws->fd = open(ws->filename, _O_WRONLY | _O_BINARY | _O_CREAT, 0644);
#else
  ws->fd = open(ws->filename, O_WRONLY | O_CREAT, 0644);
#endif
  return ws;
}

void writeable_pull_file_stream_write(writeable_pull_file_stream_t* stream, buffer_t* data) {
  REFERENCE(data, buffer_t);
  write(stream->fd, data->data, data->size);
  DESTROY(data, buffer);
}
void writeable_pull_file_stream_destroy(writeable_pull_file_stream_t* stream) {
  refcounter_dereference((refcounter_t*) stream);
  if (refcounter_count((refcounter_t*) stream) == 0) {
#ifdef _WIN32
    _close(stream->fd);
#else
    close(stream->fd);
#endif
    stream_deinit((stream_t*) stream);
    free(stream->filename);
    free(stream);
  }
}
void writeable_pull_file_stream_close(writeable_pull_file_stream_t* stream) {
  platform_lock(&stream->stream.lock);
  uint8_t deactivated = stream->stream.is_deactivated;
  if (deactivated == 0) {
    stream->stream.is_deactivated = 1;
    stream->stream.is_piped = 0;
    platform_unlock(&stream->stream.lock);
    stream_unsubscribe_pipe_notifiers((stream_t*) stream);
    stream_notify((stream_t *) stream, close_event, NULL);
  } else {
    platform_unlock(&stream->stream.lock);
  }
}