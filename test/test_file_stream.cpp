#include <gtest/gtest.h>
#include <functional>
#include <gmock/gmock.h>
#include <future>
extern "C" {
#include "../src/Streams/file-stream.h"
#include "../src/Util/path_join.h"
#include "../src/Util/mkdir_p.h"
#include "../src/Util/rm_rf.h"
#include "../src/Configuration/config.h"
#include "../src/Workers/priority.h"
#include "../src/Util/allocator.h"
}

using ::testing::_;
using ::testing::MockFunction;
using ::testing::AtLeast;

class TestFileStream : public testing::Test {
public:
  work_pool_t* pool;
  std::promise<void> r_close_promise;
  std::promise<void> r_complete_promise;
  std::promise<void> w_close_promise;
  MockFunction<void((void*, void*))> mock_data_callback;
  void SetUp() override {
    pool = work_pool_create(4);
    work_pool_launch(pool);
  }
  void TearDown() override {
    work_pool_shutdown(pool);
    work_pool_join_all(pool);
    work_pool_destroy(pool);
  }
};
void on_data(void* ctx, buffer_t* buffer) {
  REFERENCE(buffer, buffer_t);
  auto testFS = static_cast<TestFileStream*>(ctx);
  testFS->mock_data_callback.Call(ctx, (void*) buffer);
  DESTROY(buffer, buffer);
}
void on_close_r(void* ctx, void*) {
  auto testFS = static_cast<TestFileStream*>(ctx);
  testFS->r_close_promise.set_value();
}
void on_complete_r(void* ctx, void*) {
  auto testFS = static_cast<TestFileStream*>(ctx);
  testFS->r_complete_promise.set_value();
}
void on_close_w(void* ctx, void*) {
  auto testFS = static_cast<TestFileStream*>(ctx);
  testFS->w_close_promise.set_value();
}
void on_error_r(void* ctx,  async_error_t* error) {
  auto testFS = static_cast<TestFileStream*>(ctx);
  try {
    throw std::runtime_error((const char*)error->message);
  } catch(...) {
    testFS->r_close_promise.set_exception(std::current_exception());
  }
}
void on_error_w(void* ctx,  async_error_t* error) {
  auto testFS = static_cast<TestFileStream*>(ctx);
  try {
    throw std::runtime_error((const char*)error->message);
  } catch(...) {
    testFS->w_close_promise.set_exception(std::current_exception());
  }
}
TEST_F(TestFileStream, TestReadFileStreamFunctions) {
  priority_t priority = priority_get_next();
  int error_code;
  std::string filename = "./test.pdf";
  /*EXPECT_CALL(mock_data_callback, Call(_,_)).Times(23);
  readable_file_stream_t* rs = readable_file_stream_create(&priority, pool, (char*) filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
  EXPECT_EQ(error_code, 0);
  if(error_code != 0) {
    GTEST_FATAL_FAILURE_("Stream Creation error");
  }
  stream_subscribe((stream_t*) rs, error_event, this, (void(*)(void*, void*)) on_error_r, NULL);
  stream_subscribe((stream_t*) rs, close_event, this, on_close_r, NULL );
  stream_subscribe((stream_t*) rs, complete_event, this, (void(*)(void*, void*)) on_complete_r, NULL);
  stream_subscribe((stream_t*) rs, data_event, this, (void(*)(void*, void*)) on_data, NULL);
  std::future<void> r_close_future = r_close_promise.get_future();
  std::future<void> r_complete_future = r_complete_promise.get_future();
  try {
    r_close_future.get();
    EXPECT_EQ(r_complete_future.valid(), true);
  } catch (const std::exception& e) {
    GTEST_FATAL_FAILURE_(e.what());
  }
  DESTROY(rs, readable_file_stream);*/
  readable_file_stream_t* rs = readable_file_stream_create(&priority, pool, (char*) filename.c_str(), DEFAULT_CHUNK_SIZE, &error_code);
  EXPECT_EQ(error_code, 0);
  EXPECT_EQ(rs->stream.type, readable_stream);
  if(error_code != 0) {
    GTEST_FATAL_FAILURE_("Stream Creation error");
  }
  /*
  std::future<void> w_close_future = w_close_promise.get_future();
  std::string filename2 = "./test2.pdf";
  writeable_file_stream_t*  ws = writeable_file_stream_create(priority, pool, (char*) filename2.c_str());
  stream_subscribe((stream_t*) ws, error_event, this, (void(*)(void*, void*)) on_error_w, NULL);
  stream_subscribe((stream_t*) ws, close_event, this, (void(*)(void*, void*)) on_close_w, NULL);
  readable_push_stream_pipe((stream_t*) rs, (stream_t*) ws);
  try {
    w_close_future.get();
  } catch (const std::exception& e) {
    GTEST_FATAL_FAILURE_(e.what());
  }*/
  DESTROY(rs, readable_file_stream);
  //DESTROY(ws, writeable_file_stream);
};