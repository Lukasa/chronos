#include "timer_store.h"
#include "log.h"

#include <algorithm>
#include <time.h>

TimerStore::TimerStore() : _current_ms_bucket(0),
                           _current_s_bucket(0)
{
  update_first_bucket();
}

TimerStore::~TimerStore()
{
  // Delete the timers in the lookup table as they will never pop now.
  for (auto it = _timer_lookup_table.begin(); it != _timer_lookup_table.end(); it++)
  {
    delete it->second;
  }
  _timer_lookup_table.clear();
  for (int ii = 0; ii < 100; ii ++)
  {
    _ten_ms_buckets[ii].clear();
  }
  for (int ii = 0; ii < NUM_SECOND_BUCKETS; ii++)
  {
    _s_buckets[ii].clear();
  }
}

// Give a timer to the data store.  At this point the data store takes ownership of
// the timer and the caller should not reference it again (as the timer store may
// delete it at any time).
void TimerStore::add_timer(Timer* t)
{
  // First check if this timer already exists.
  auto map_it = _timer_lookup_table.find(t->id);
  if (map_it != _timer_lookup_table.end())
  {
    Timer* existing = map_it->second;

    // Compare timers for precedence, start-time then sequence-number.
    if ((t->start_time < existing->start_time) ||
        ((t->start_time == existing->start_time) &&
         (t->sequence_number < existing->sequence_number)))
    {
      // Existing timer is more recent
      delete t;
      return;
    }
    else
    {
      // Existing timer is older
      if (t->is_tombstone())
      {
        // Learn the interval so that this tombstone lasts long enough to catch errors.
        t->interval = existing->interval;
        t->repeat_for = existing->interval;
      }
      delete_timer(t->id);
    }
  }

  std::unordered_set<Timer*>* bucket = find_bucket_from_timer(t);
  if (bucket)
  {
    bucket->insert(t);
  }
  else
  {
    // Timer is too far in the future to be handled by the buckets, put it in the
    // extra heap.
    LOG_WARNING("Adding timer to extra heap, consider re-building with a larger "
                "NUM_SECOND_BUCKETS constant");
    _extra_heap.push_back(t);
    std::push_heap(_extra_heap.begin(), _extra_heap.end());
  }

  // Finally, add the timer to the lookup table.
  _timer_lookup_table.insert(std::pair<TimerID, Timer*>(t->id, t));
}

// Add a collection of timers to the data store.  The collection is emptied by this
// operation, since the timers are now owned by the store.
void TimerStore::add_timers(std::unordered_set<Timer*>& set)
{
  for (auto it = set.begin(); it != set.end(); it++)
  {
    Timer* t = *it;
    add_timer(t);
  }
  set.clear();
}

// Delete a timer from the store by ID.
void TimerStore::delete_timer(TimerID id)
{
  std::map<TimerID, Timer*>::iterator it;
  it = _timer_lookup_table.find(id);
  if (it != _timer_lookup_table.end())
  {
    // The timer is still present in the store, delete it.
    Timer* timer = it->second;
    std::unordered_set<Timer*>* bucket = find_bucket_from_timer(timer);
    if (bucket)
    {
      bucket->erase(timer);
    }
    else
    {
      std::vector<Timer*>::iterator heap_it;
      heap_it = std::find(_extra_heap.begin(), _extra_heap.end(), timer);
      if (heap_it != _extra_heap.end())
      {
        // Timer is in heap, remove it.
        _extra_heap.erase(heap_it, heap_it + 1);
        std::make_heap(_extra_heap.begin(), _extra_heap.end());
      }
    }
    _timer_lookup_table.erase(id);
    delete timer;
  }
}

// Retrieve the set of timers to pop in the next 10ms.  The timers returned are
// disowned by the store and must be freed by the caller or returned to the store
// through `add_timer()`.
//
// If the returned set is empty, there are no timers in the store and the caller
// will try again later (after a signal that a new timer has been added).
void TimerStore::get_next_timers(std::unordered_set<Timer*>& set)
{
  // If there are no timers, simply return an empty set.
  if (_timer_lookup_table.empty())
  {
    return;
  }

  // The store is not empty, find the first set that will pop.
  while (_ten_ms_buckets[_current_ms_bucket].empty())
  {
    if (_current_ms_bucket >= 99)
    {
      refill_ms_buckets();
    }
    else
    {
      _current_ms_bucket++;
    }
  }

  // Remove the timers from the lookup table, and pass ownership of the
  // memory for the timers to the caller.
  for (auto it = _ten_ms_buckets[_current_ms_bucket].begin();
       it != _ten_ms_buckets[_current_ms_bucket].end();
       it++)
  {
    _timer_lookup_table.erase((*it)->id);
    set.insert(*it);
  }
  _ten_ms_buckets[_current_ms_bucket].clear();
}

void TimerStore::update_first_bucket()
{
  struct timespec ts;

  if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
  {
    perror("Failed to get system time - timer service cannot run: ");
    exit(-1);
  }

  _first_bucket_timestamp = (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

/*****************************************************************************/
/* Private functions.                                                        */
/*****************************************************************************/

// Moves timers from the next seconds bucket to the 10ms buckets and resets the
// current 10ms bucket index.
void TimerStore::refill_ms_buckets()
{
  if (_current_s_bucket >= (NUM_SECOND_BUCKETS - 1))
  {
    refill_s_buckets();
  }

  // Update timing records, at this point, time advances by 1 second.
  _current_ms_bucket = 0;
  _first_bucket_timestamp += 1000;

  // Distribute the next second bucket into the ms buckets.
  distribute_s_bucket(_current_s_bucket++);
}

// Moves timers from a given second bucket into the appropriate 10ms bucket.
void TimerStore::distribute_s_bucket(unsigned int index)
{
  for (auto it = _s_buckets[index].begin(); it != _s_buckets[index].end(); it++)
  {
    std::unordered_set<Timer*>* bucket = find_bucket_from_timer(*it);
    bucket->insert(*it);
  }
  _s_buckets[index].clear();
}

// Moves timers from the extra heap to the seconds buckets and resets the current
// seconds bucket index.
void TimerStore::refill_s_buckets()
{
  // Reset the second buckets to the beginning.
  _current_s_bucket = 0;

  if (!_extra_heap.empty())
  {
    std::pop_heap(_extra_heap.begin(), _extra_heap.end());
    Timer* timer = _extra_heap.back();

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    uint64_t next_pop_time = ts.tv_sec * 1000 + 1000;
    next_pop_time += ts.tv_nsec / 1000000;

    while ((timer != NULL) &&
           (timer->next_pop_time() - _first_bucket_timestamp) < 3600 * 1000)
    {
      // Remove timer from heap
      _extra_heap.pop_back();

      std::unordered_set<Timer*>* bucket = find_bucket_from_timer(timer);
      bucket->insert(timer);

      if (!_extra_heap.empty())
      {
        std::pop_heap(_extra_heap.begin(), _extra_heap.end());
        timer = _extra_heap.back();
      }
      else
      {
        timer = NULL;
      }
    }

    // Push the timer back into the heap.
    if (!_extra_heap.empty())
    {
      std::push_heap(_extra_heap.begin(), _extra_heap.end());
    }
  }

  _current_s_bucket = 0;
}

// Calculate which bucket the given timer belongs in, based on the next pop time
// and the store's current view of the clock.
//
// If the timer would be stored in the heap, this function returns NULL.
std::unordered_set<Timer*>* TimerStore::find_bucket_from_timer(Timer* t)
{
  // Calculate how long till the timer will pop.
  uint64_t next_pop_timestamp = t->next_pop_time();
  uint64_t time_to_next_pop;
  if (next_pop_timestamp < _first_bucket_timestamp)
  {
    // Timer should have already popped.  Best we can do is put it in the very first
    // available bucket so it gets popped as soon as possible.
    LOG_WARNING("Modifying timer after pop time, window condition detected");
    time_to_next_pop = 0;
  }
  else
  {
    time_to_next_pop = next_pop_timestamp - _first_bucket_timestamp;
  }

  // Now find the bucket for the timer.
  if (time_to_next_pop < 1000)
  {
    return &_ten_ms_buckets[time_to_next_pop / 10];
  }
  else if (time_to_next_pop < 1000 * NUM_SECOND_BUCKETS)
  {
    // Note, the seconds buckets are offset by one (to account for the millisecond
    // buckets taking up the first second's worth of time).
    return &_s_buckets[(time_to_next_pop / 1000) - 1];
  }

  return NULL;
}
