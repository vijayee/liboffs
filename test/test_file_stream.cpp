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

class TestReadFileStream : public testing::Test {
public:
  work_pool_t* pool;
  std::promise<void> close_promise;
  std::promise<void> complete_promise;
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
  auto testRS = static_cast<TestReadFileStream*>(ctx);
  testRS->mock_data_callback.Call(ctx, (void*) buffer);
  DESTROY(buffer, buffer);
}
void on_close(void* ctx, void*) {
  auto testRS = static_cast<TestReadFileStream*>(ctx);
  testRS->close_promise.set_value();
}
void on_complete(void* ctx, void*) {
  auto testRS = static_cast<TestReadFileStream*>(ctx);
  testRS->complete_promise.set_value();
}
void on_error(void* ctx,  async_error_t* error) {
  auto testRS = static_cast<TestReadFileStream*>(ctx);
  try {
    throw std::runtime_error((const char*)error->message);
  } catch(...) {
    testRS->close_promise.set_exception(std::current_exception());
  }

}
TEST_F(TestReadFileStream, TestReadFileStreamFunctions) {
  priority_t priority = priority_get_next();
  int error_code;
  char* filename = "./test.pdf";
  EXPECT_CALL(mock_data_callback, Call(_,_)).Times(25);
  readable_file_stream_t* rs = readable_file_stream_create(priority, pool, filename, DEFAULT_CHUNK_SIZE, &error_code);
  EXPECT_EQ(error_code, 0);
  if(error_code != 0) {
    GTEST_FATAL_FAILURE_("Stream Creation error");
  }
  stream_subscribe((stream_t*) rs, error_event, this, (void(*)(void*, void*)) on_error, NULL);
  stream_subscribe((stream_t*) rs, close_event, this, on_close, NULL );
  stream_subscribe((stream_t*) rs, data_event, this, (void(*)(void*, void*)) on_data, NULL);
  std::future<void> close_future = close_promise.get_future();
  std::future<void> complete_future = complete_promise.get_future();
  try {
    close_future.get();
    EXPECT_EQ(complete_future.valid(), true);
  } catch (const std::exception& e) {
    GTEST_FATAL_FAILURE_(e.what());
  }
  DESTROY(rs, readable_file_stream);
};