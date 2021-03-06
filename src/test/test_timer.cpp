#include "timer.h"
#include "globals.h"
#include "base.h"

#include <gtest/gtest.h>
#include <map>

/*****************************************************************************/
/* Test fixture                                                              */
/*****************************************************************************/

class TestTimer : public Base
{
protected:
  virtual void SetUp()
  {
    Base::SetUp();
    std::vector<std::string> replicas;
    replicas.push_back("10.0.0.1");
    replicas.push_back("10.0.0.2");
    TimerID id = (TimerID)UINT_MAX + 10;
    uint32_t interval = 100;
    uint32_t repeat_for = 200;

    t1 = new Timer(id, interval, repeat_for);
    t1->start_time = 1000000;
    t1->sequence_number = 0;
    t1->replicas = replicas;
    t1->callback_url = "http://localhost:80/callback";
    t1->callback_body = "stuff stuff stuff";
  }

  virtual void TearDown()
  {
    delete t1;
    Base::TearDown();
  }

  // Helper function to access timer private variables
  int get_replication_factor(Timer* t) { return t->_replication_factor; }

  Timer* t1;
};

/*****************************************************************************/
/* Class functions                                                           */
/*****************************************************************************/

TEST_F(TestTimer, FromJSONTests)
{
  std::vector<std::string> failing_test_data;
  failing_test_data.push_back(
      "{}");
  failing_test_data.push_back(
      "{\"timing\"}");
  failing_test_data.push_back(
      "{\"timing\": []}");
  failing_test_data.push_back(
      "{\"timing\": [], \"callback\": []}");
  failing_test_data.push_back(
      "{\"timing\": [], \"callback\": [], \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": {}, \"callback\": [], \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": \"hello\" }, \"callback\": [], \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": \"hello\", \"repeat-for\": \"hello\" }, \"callback\": [], \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": \"hello\" }, \"callback\": [], \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": [], \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": {}, \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": []}, \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": {}}, \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": [] }}, \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": [], \"opaque\": [] }}, \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": [] }}, \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": []}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replication-factor\": \"hello\" }}");
  failing_test_data.push_back(
      "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replicas\": [] }}");

  // Reliability can be ignored by the client to use default replication.
  std::string default_repl_factor = "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}}";

  // Reliability can be specified as empty by the client to use default replication.
  std::string default_repl_factor2 = "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": {}}";

  // Or you can pass a custom replication factor.
  std::string custom_repl_factor = "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replication-factor\": 3 }}";

  // Or you can pass specific replicas to use.
  std::string specific_replicas = "{\"timing\": { \"interval\": 100, \"repeat-for\": 200 }, \"callback\": { \"http\": { \"uri\": \"localhost\", \"opaque\": \"stuff\" }}, \"reliability\": { \"replicas\": [ \"10.0.0.1\", \"10.0.0.2\" ] }}";

  // Each of the failing json blocks should not parse to a timer.
  for (auto it = failing_test_data.begin(); it != failing_test_data.end(); it++)
  {
    std::string err;
    bool replicated;
    EXPECT_EQ((void*)NULL, Timer::from_json(1, 0, *it, err, replicated)) << *it;
    EXPECT_NE("", err);
  }

  std::string err;
  bool replicated;
  Timer* timer;

  // If you don't specify a replication-factor, use 2.
  timer = Timer::from_json(1, 0, default_repl_factor, err, replicated);
  EXPECT_NE((void*)NULL, timer);
  EXPECT_EQ("", err);
  EXPECT_FALSE(replicated);
  EXPECT_EQ(2, get_replication_factor(timer));
  EXPECT_EQ(2, timer->replicas.size());
  delete timer;
  timer = Timer::from_json(1, 0, default_repl_factor2, err, replicated);
  EXPECT_NE((void*)NULL, timer);
  EXPECT_EQ("", err);
  EXPECT_FALSE(replicated);
  EXPECT_EQ(2, get_replication_factor(timer));
  EXPECT_EQ(2, timer->replicas.size());
  delete timer;

  // If you do specify a replication-factor, use that.
  timer = Timer::from_json(1, 0, custom_repl_factor, err, replicated);
  EXPECT_NE((void*)NULL, timer);
  EXPECT_EQ("", err);
  EXPECT_FALSE(replicated);
  EXPECT_EQ(3, get_replication_factor(timer));
  EXPECT_EQ(3, timer->replicas.size());
  delete timer;

  // Get the replicas from the bloom filter if given
  timer = Timer::from_json(1, 0x11011100011101, default_repl_factor, err, replicated);
  EXPECT_NE((void*)NULL, timer);
  EXPECT_EQ("", err);
  EXPECT_FALSE(replicated);
  EXPECT_EQ(2, get_replication_factor(timer));
  delete timer;

  timer = Timer::from_json(1, 0x11011100011101, custom_repl_factor, err, replicated);
  EXPECT_NE((void*)NULL, timer);
  EXPECT_EQ("", err);
  EXPECT_FALSE(replicated);
  EXPECT_EQ(3, get_replication_factor(timer));
  delete timer;

  // If specifc replicas are specified, use them (regardless of presence of bloom hash).
  timer = Timer::from_json(1, 0x11011100011101, specific_replicas, err, replicated);
  EXPECT_NE((void*)NULL, timer);
  EXPECT_EQ("", err);
  EXPECT_TRUE(replicated);
  EXPECT_EQ(2, get_replication_factor(timer));
  delete timer;
}

// Utility thread function to test thread-safeness of the unique generation
// algorithm.
void* generate_ids(void* arg)
{
  std::vector<TimerID>* output = (std::vector<TimerID>*)arg;
  for (int ii = 0; ii < 1000; ii++)
  {
    TimerID t = Timer::generate_timer_id();
    output->push_back(t);
  }

  std::sort(output->begin(), output->end());

  return NULL;
}

TEST_F(TestTimer, GenerateTimerIDTests)
{
  const int concurrency = 50;

  pthread_t thread_ids[concurrency];
  std::vector<TimerID> ids[concurrency];
  std::vector<TimerID> all_ids;

  // Generate multiple (sorted) arrays of IDs in multiple threads.
  for (int ii = 0; ii < concurrency; ii++)
  {
    ASSERT_EQ(0, pthread_create(&thread_ids[ii], NULL, generate_ids, &ids[ii]));
  }

  // Wait for the threads to finish.
  for (int ii = 0; ii < concurrency; ii++)
  {
    ASSERT_EQ(0, pthread_join(thread_ids[ii], NULL));
  }

  // Merge all the (sorted) ID lists together.
  for (int ii = 0; ii < concurrency; ii++)
  {
    int midpoint = all_ids.size();
    all_ids.insert(all_ids.end(), ids[ii].begin(), ids[ii].end());
    std::inplace_merge(all_ids.begin(), all_ids.begin() + midpoint, all_ids.end());
  }

  // Assert that no pairs are equal.
  for(int ii = 1; ii < concurrency * 1000; ii++)
  {
    EXPECT_NE(all_ids[ii], all_ids[ii-1]);
  }
}

/*****************************************************************************/
/* Instance Functions                                                        */
/*****************************************************************************/

TEST_F(TestTimer, URL)
{
  EXPECT_EQ("http://hostname:9999/timers/00000001000000090010011000011001", t1->url("hostname"));
}

TEST_F(TestTimer, ToJSON)
{
  // Test this by rendering as JSON, then parsing back to a timer
  // and comparing.

  // We need to use a new timer here, because the values we use in
  // testing (100ms and 200ms) are too short to be specified on the
  // JSON interface (which counts in seconds).
  uint32_t interval = 1000;
  uint32_t repeat_for = 2000;

  Timer* t2 = new Timer(1, interval, repeat_for);
  t2->start_time = 1000000;
  t2->sequence_number = 0;
  t2->replicas = t1->replicas;
  t2->callback_url = "http://localhost:80/callback";
  t2->callback_body = "{\"stuff\": \"stuff\"}";

  std::string json = t2->to_json();
  std::string err;
  bool replicated;

  Timer* t3 = Timer::from_json(2, 0, json, err, replicated);
  EXPECT_EQ(err, "");
  EXPECT_TRUE(replicated);
  ASSERT_NE((void*)NULL, t2);

  EXPECT_EQ(2, t3->id) << json;
  EXPECT_EQ(1000000, t3->start_time) << json;
  EXPECT_EQ(t2->interval, t3->interval) << json;
  EXPECT_EQ(t2->repeat_for, t3->repeat_for) << json;
  EXPECT_EQ(2, get_replication_factor(t3)) << json;
  EXPECT_EQ(t2->replicas, t3->replicas) << json;
  EXPECT_EQ("http://localhost:80/callback", t3->callback_url) << json;
  EXPECT_EQ("{\"stuff\": \"stuff\"}", t3->callback_body) << json;
  delete t2;
  delete t3;
}

TEST_F(TestTimer, IsLocal)
{
  EXPECT_TRUE(t1->is_local("10.0.0.1"));
  EXPECT_FALSE(t1->is_local("20.0.0.1"));
}

TEST_F(TestTimer, IsTombstone)
{
  Timer* t2 = Timer::create_tombstone(100, 0);
  EXPECT_NE(0, t2->start_time);
  EXPECT_TRUE(t2->is_tombstone());
  delete t2;
}

TEST_F(TestTimer, BecomeTombstone)
{
  EXPECT_FALSE(t1->is_tombstone());
  t1->become_tombstone();
  EXPECT_TRUE(t1->is_tombstone());
  EXPECT_EQ(1000000, t1->start_time);
  EXPECT_EQ(100, t1->interval);
  EXPECT_EQ(100, t1->repeat_for);
}
