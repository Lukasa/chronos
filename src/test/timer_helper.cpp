#include "timer_helper.h"

Timer* default_timer(TimerID id)
{
  Timer* timer = new Timer(id, 100, 100);
  timer->start_time = 1000000;
  timer->sequence_number = 0;
  timer->replicas = std::vector<std::string>(1, "10.0.0.1");
  timer->callback_url = "localhost:80/callback" + std::to_string(id);
  timer->callback_body = "stuff stuff stuff";
  return timer;
}
