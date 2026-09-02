#include "codexion.h"

long	wall_ms(void)
{
	struct timeval	tv;

	gettimeofday(&tv, NULL);
	return ((long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

long	now_ms(t_lab *lab)
{
	return (wall_ms() - lab->start_time);
}

static void	timespec_from_now(long delay_ms, struct timespec *ts)
{
	struct timeval	now;
	long			nsec;

	gettimeofday(&now, NULL);
	nsec = now.tv_usec * 1000 + (delay_ms % 1000) * 1000000;
	ts->tv_sec = now.tv_sec + delay_ms / 1000 + nsec / 1000000000;
	ts->tv_nsec = nsec % 1000000000;
}

void	wait_ms(t_coder *coder, long duration)
{
	t_lab			*lab;
	long			deadline;
	long			remaining;
	struct timespec	ts;

	lab = coder->lab;
	deadline = now_ms(lab) + duration;
	pthread_mutex_lock(&lab->sched_mutex);
	while (!lab->stopped)
	{
		remaining = deadline - now_ms(lab);
		if (remaining <= 0)
			break ;
		timespec_from_now(remaining, &ts);
		pthread_cond_timedwait(&coder->cond, &lab->sched_mutex, &ts);
	}
	pthread_mutex_unlock(&lab->sched_mutex);
}
