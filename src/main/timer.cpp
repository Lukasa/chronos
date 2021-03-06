#include "timer.h"
#include "globals.h"
#include "murmur/MurmurHash3.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "utils.h"
#include "log.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <boost/format.hpp>
#include <map>
#include <atomic>

Timer::Timer(TimerID id, uint32_t interval, uint32_t repeat_for) :
  id(id),
  interval(interval),
  repeat_for(repeat_for),
  sequence_number(0),
  replicas(std::vector<std::string>()),
  callback_url(""),
  callback_body(""),
  _replication_factor(0)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  start_time = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

Timer::~Timer()
{
}

// Returns the next pop time in ms.
uint64_t Timer::next_pop_time()
{
  std::string localhost;
  int replica_index = 0;
  __globals->get_cluster_local_ip(localhost);

  for (auto it = replicas.begin(); it != replicas.end(); it++, replica_index++)
  {
    if (*it == localhost)
    {
      break;
    }
  }

  return start_time + ((sequence_number + 1) * interval) + (replica_index * 2 * 1000);
}

// Create the timer's URL from a given hostname
std::string Timer::url(std::string host)
{
  std::stringstream ss;

  int bind_port;
  __globals->get_bind_port(bind_port);

  // Here (and below) we render the timer ID (and replica hash) as 0-padded
  // hex strings so we can parse it back out later easily.
  ss << "http://" << host << ":" << bind_port << "/timers/";
  ss << std::setfill('0') << std::setw(16) << std::hex << id;
  uint64_t hash = 0;
  std::map<std::string, uint64_t> cluster_hashes;
  __globals->get_cluster_hashes(cluster_hashes);
  for (auto it = replicas.begin(); it != replicas.end(); it++)
  {
    hash |= cluster_hashes[*it];
  }
  ss << std::setfill('0') << std::setw(16) << std::hex << hash;


  return ss.str();
}

// Render the timer as JSON to be used in an HTTP request body.
// The JSON should take the form:
// {
//     "timing": {
//         "start-time": Int64,
//         "sequence-number": Int,
//         "interval": Int,
//         "repeat-for": Int
//     },
//     "callback": {
//         "http": {
//             "uri": "string",
//             "opaque": "string"
//         }
//     },
//     "reliability": {
//         "replicas": [
//             <comma separated "string"s>
//         ]
//     }
// }
std::string Timer::to_json()
{
  rapidjson::Document doc;
  doc.SetObject();

  rapidjson::Value timing(rapidjson::kObjectType);
  timing.AddMember("start-time", start_time, doc.GetAllocator());
  timing.AddMember("sequence-number", sequence_number, doc.GetAllocator());
  timing.AddMember("interval", interval/1000, doc.GetAllocator());
  timing.AddMember("repeat-for", repeat_for/1000, doc.GetAllocator());

  rapidjson::Value http(rapidjson::kObjectType);
  http.AddMember("uri", callback_url.c_str(), doc.GetAllocator());
  http.AddMember("opaque", callback_body.c_str(), doc.GetAllocator());

  rapidjson::Value callback(rapidjson::kObjectType);
  callback.AddMember("http", http, doc.GetAllocator());

  rapidjson::Value replicas_array(rapidjson::kArrayType);
  for (auto it = replicas.begin(); it != replicas.end(); it++)
  {
    replicas_array.PushBack((*it).c_str(), doc.GetAllocator());
  }

  rapidjson::Value reliability(rapidjson::kObjectType);
  reliability.AddMember("replicas", replicas_array, doc.GetAllocator());

  // Create the document.
  doc.AddMember("timing", timing, doc.GetAllocator());
  doc.AddMember("callback", callback, doc.GetAllocator());
  doc.AddMember("reliability", reliability, doc.GetAllocator());

  rapidjson::StringBuffer s;
  rapidjson::Writer<rapidjson::StringBuffer> w(s);
  doc.Accept(w);
  std::string body = s.GetString();

  LOG_DEBUG("Built replication body: %s", body.c_str());

  return body;
}

bool Timer::is_local(std::string host)
{
  return (std::find(replicas.begin(), replicas.end(), host) != replicas.end());
}

bool Timer::is_tombstone()
{
  return ((callback_url == "") && (callback_body == ""));
}

void Timer::become_tombstone()
{
  callback_url = "";
  callback_body = "";

  // Since we're not bringing the start-time forward we have to extend the
  // repeat-for to ensure the tombstone gets added to the replica's store.
  repeat_for = interval * (sequence_number + 1);
}

void Timer::calculate_replicas(uint64_t replica_hash)
{
  std::vector<std::string> hash_replicas;
  if (replica_hash)
  {
    // Compare the hash to all the known replicas looking for matches.
    std::map<std::string, uint64_t> cluster_hashes;
    __globals->get_cluster_hashes(cluster_hashes);

    for (auto it = cluster_hashes.begin();
         it != cluster_hashes.end();
         it++)
    {
      // Quickly check if this replica might be one of the replicas for the
      // given timer (i.e. if the replica's individual hash collides with the
      // bloom filter we calculated when we created the hash (see `url()`).
      if ((replica_hash & it->second) == it->second)
      {
        // This is probably a replica.
        hash_replicas.push_back(it->first);
      }
    }

    // Recreate the vector of replicas. Use the replication factor if it's set,
    // otherwise use the size of the existing replicas.
    _replication_factor = _replication_factor > 0 ?
                          _replication_factor : hash_replicas.size();
    uint32_t hash;
    MurmurHash3_x86_32(&id, sizeof(TimerID), 0x0, &hash);
    std::vector<std::string> cluster;
    __globals->get_cluster_addresses(cluster);
    unsigned int first_replica = hash % cluster.size();

    for (unsigned int ii = 0;
         ii < _replication_factor && ii < cluster.size();
         ii++)
    {
      replicas.push_back(cluster[(first_replica + ii) % cluster.size()]);
    }

    // Finally, add any replicas that were in hash_replicas but aren't in
    // replicas to the extra_replicas vector.
    for (unsigned int ii = 0;
         ii < hash_replicas.size();
         ii++)
    {
      if (std::find(replicas.begin(), replicas.end(), hash_replicas[ii]) == replicas.end())
      {
        extra_replicas.push_back(hash_replicas[ii]);
      }
    }
  }
  else
  {
    // Pick replication-factor replicas from the cluster, using a hash of the ID
    // to balance the choices.
    uint32_t hash;
    MurmurHash3_x86_32(&id, sizeof(TimerID), 0x0, &hash);
    std::vector<std::string> cluster;
    __globals->get_cluster_addresses(cluster);
    unsigned int first_replica = hash % cluster.size();
    for (unsigned int ii = 0;
         ii < _replication_factor && ii < cluster.size();
         ii++)
    {
      replicas.push_back(cluster[(first_replica + ii) % cluster.size()]);
    }
  }

  LOG_DEBUG("Replicas calculated:");
  for (auto it = replicas.begin(); it != replicas.end(); it++)
  {
    LOG_DEBUG(" - %s", it->c_str());
  }
}

uint32_t Timer::deployment_id = 0;
uint32_t Timer::instance_id = 0;

// Generate a timer that should be unique across the (possibly geo-redundant) cluster.
// The idea is to use a combination of deployment id, instance id, timestamp and
// an incrementing sequence number.
//
// The ID returned to the client will also contain a
// list of replicas, but this doesn't add much uniqueness.
TimerID Timer::generate_timer_id()
{
  return (TimerID)Utils::generate_unique_integer(Timer::deployment_id,
                                                 Timer::instance_id);
}

// Created tombstones from delete operations are given
// default expires of 10 seconds, if they're found to be
// deleting an existing tombstone, they'll use that timer's
// interval as an expiry.
Timer* Timer::create_tombstone(TimerID id, uint64_t replica_hash)
{
  // Create a tombstone record that will last for 10 seconds.
  Timer* tombstone = new Timer(id, 10000, 10000);
  tombstone->calculate_replicas(replica_hash);
  return tombstone;
}

#define JSON_PARSE_ERROR(STR) {                                               \
  error = (STR);                                                              \
  delete timer;                                                               \
  return NULL;                                                                \
}

#define JSON_ASSERT_OBJECT(NODE, NODE_NAME) {                                 \
  if (!(NODE).IsObject())                                                     \
    JSON_PARSE_ERROR((NODE_NAME " should be an object"));                     \
}

#define JSON_ASSERT_INTEGER(NODE, NODE_NAME) {                                \
  if (!(NODE).IsInt())                                                        \
    JSON_PARSE_ERROR((NODE_NAME " should be an integer"));                    \
}

#define JSON_ASSERT_INTEGER_64(NODE, NODE_NAME) {                             \
  if (!(NODE).IsInt64())                                                      \
    JSON_PARSE_ERROR((NODE_NAME " should be an 64bit integer"));              \
}

#define JSON_ASSERT_STRING(NODE, NODE_NAME) {                                 \
  if (!(NODE).IsString())                                                     \
    JSON_PARSE_ERROR((NODE_NAME " should be a string"));                      \
}

#define JSON_ASSERT_ARRAY(NODE, NODE_NAME) {                                  \
  if (!(NODE).IsArray())                                                      \
    JSON_PARSE_ERROR((NODE_NAME " should be an array"));                      \
}

#define JSON_ASSERT_CONTAINS(NODE, NODE_NAME, ELEM) {                         \
  if (!(NODE).HasMember(ELEM))                                                \
    JSON_PARSE_ERROR(("Couldn't find '" ELEM "' in '" NODE_NAME "'"));        \
}

// Create a Timer object from the JSON representation.
//
// @param id - The unique identity for the timer (see generate_timer_id() above).
// @param replica_hash - The replica hash extracted from the timer URL (or 0 for new timer).
// @param json - The JSON representation of the timer.
// @param error - This will be populated with a descriptive error string if required.
// @param replicated - This will be set to true if this is a replica of a timer.
Timer* Timer::from_json(TimerID id, uint64_t replica_hash, std::string json, std::string& error, bool& replicated)
{
  Timer* timer = NULL;
  rapidjson::Document doc;
  doc.Parse<0>(json.c_str());
  if (doc.HasParseError())
  {
    JSON_PARSE_ERROR(boost::str(boost::format("Failed to parse JSON body, offset: %lu - %s. JSON is: %s") % doc.GetErrorOffset() % doc.GetParseError() % json));
  }

  if (!doc.HasMember("timing"))
    JSON_PARSE_ERROR(("Couldn't find the 'timing' node in the JSON"));
  if (!doc.HasMember("callback"))
    JSON_PARSE_ERROR(("Couldn't find the 'callback' node in the JSON"));

  // Parse out the timing block
  rapidjson::Value& timing = doc["timing"];

  JSON_ASSERT_OBJECT(timing, "timing");
  JSON_ASSERT_CONTAINS(timing, "timing", "interval");
  JSON_ASSERT_CONTAINS(timing, "timing", "repeat-for");

  rapidjson::Value& interval = timing["interval"];
  rapidjson::Value& repeat_for = timing["repeat-for"];

  JSON_ASSERT_INTEGER(interval, "interval");
  JSON_ASSERT_INTEGER(repeat_for, "repeat-for");

  timer = new Timer(id, (interval.GetInt() * 1000), (repeat_for.GetInt() * 1000));

  if (timing.HasMember("start-time"))
  {
    // Timer JSON specifies a start-time, use that instead of now.
    rapidjson::Value& start_time = timing["start-time"];
    JSON_ASSERT_INTEGER_64(start_time, "start-time");
    timer->start_time = start_time.GetInt64();
  }

  if (timing.HasMember("sequence-number"))
  {
    rapidjson::Value& sequence_number = timing["sequence-number"];
    JSON_ASSERT_INTEGER(sequence_number, "sequence-number");
    timer->sequence_number = sequence_number.GetInt();
  }

  // Parse out the 'callback' block
  rapidjson::Value& callback = doc["callback"];

  JSON_ASSERT_OBJECT(callback, "callback");
  JSON_ASSERT_CONTAINS(callback, "callback", "http");

  rapidjson::Value& http = callback["http"];

  JSON_ASSERT_OBJECT(http, "http");
  JSON_ASSERT_CONTAINS(http, "http", "uri");
  JSON_ASSERT_CONTAINS(http, "http", "opaque");

  rapidjson::Value& uri = http["uri"];
  rapidjson::Value& opaque = http["opaque"];

  JSON_ASSERT_STRING(uri, "uri");
  JSON_ASSERT_STRING(opaque, "opaque");

  timer->callback_url = std::string(uri.GetString(), uri.GetStringLength());
  timer->callback_body = std::string(opaque.GetString(), opaque.GetStringLength());

  // Parse out the 'reliability' block
  rapidjson::Value& reliability = doc["reliability"];

  if (doc.HasMember("reliability"))
  {
    JSON_ASSERT_OBJECT(reliability, "reliability");

    if (reliability.HasMember("replicas"))
    {
      rapidjson::Value& replicas = reliability["replicas"];
      JSON_ASSERT_ARRAY(replicas, "replicas");

      if (replicas.Size() == 0)
      {
        JSON_PARSE_ERROR("If replicas is specified it must be non-empty");
      }

      timer->_replication_factor = replicas.Size();
      for (auto it = replicas.Begin(); it != replicas.End(); it++)
      {
        JSON_ASSERT_STRING(*it, "replica address");
        timer->replicas.push_back(std::string(it->GetString(), it->GetStringLength()));
      }
    }
    else
    {
      if (reliability.HasMember("replication-factor"))
      {
        rapidjson::Value& replication_factor = reliability["replication-factor"];
        JSON_ASSERT_INTEGER(replication_factor, "replication-factor");
        timer->_replication_factor = replication_factor.GetInt();
      }
      else
      {
        // Default replication factor is 2.
        timer->_replication_factor = 2;
      }
    }
  }
  else
  {
    // Default to 2 replicas
    timer->_replication_factor = 2;
  }

  if (timer->replicas.empty())
  {
    // Replicas not determined above, determine them now.  Note that this implies
    // the request is from a client, not another replica.
    replicated = false;
    timer->calculate_replicas(replica_hash);
  }
  else
  {
    // Replicas were specified in the request, must be a replication message
    // from another cluster node.
    replicated = true;
  }

  return timer;
}
