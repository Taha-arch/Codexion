#include "codexion.h"

static int	init_dongles(t_lab *lab)
{
	int	i;

	i = 0;
	while (i < lab->coders_n)
	{
		if (pthread_mutex_init(&lab->dongles[i].mutex, NULL))
		{
			while (--i >= 0)
				pthread_mutex_destroy(&lab->dongles[i].mutex);
			return (-1);
		}
		lab->dongles[i].taken = 0;
		lab->dongles[i].free_at = 0;
		i++;
	}
	return (0);
}

static int	init_coders(t_lab *lab)
{
	int	i;

	i = 0;
	while (i < lab->coders_n)
	{
		if (pthread_cond_init(&lab->coders[i].cond, NULL))
		{
			while (--i >= 0)
				pthread_cond_destroy(&lab->coders[i].cond);
			return (-1);
		}
		lab->coders[i].id = i + 1;
		lab->coders[i].left = &lab->dongles[i];
		lab->coders[i].right = &lab->dongles[(i + 1) % lab->coders_n];
		lab->coders[i].last_compile_start = -1;
		lab->coders[i].compiles_done = 0;
		lab->coders[i].ready = 0;
		lab->coders[i].lab = lab;
		i++;
	}
	return (0);
}

static int	init_sync(t_lab *lab)
{
	if (pthread_mutex_init(&lab->sched_mutex, NULL))
		return (-1);
	if (pthread_mutex_init(&lab->log_mutex, NULL))
	{
		pthread_mutex_destroy(&lab->sched_mutex);
		return (-1);
	}
	if (init_dongles(lab))
	{
		pthread_mutex_destroy(&lab->sched_mutex);
		pthread_mutex_destroy(&lab->log_mutex);
		return (-1);
	}
	if (init_coders(lab))
	{
		pthread_mutex_destroy(&lab->sched_mutex);
		pthread_mutex_destroy(&lab->log_mutex);
		return (-1);
	}
	return (0);
}

static int	spawn_threads(t_lab *lab)
{
	int	i;
	int	j;

	if (pthread_create(&lab->monitor, NULL, monitor_main, lab))
		return (-1);
	i = 0;
	while (i < lab->coders_n)
	{
		if (pthread_create(&lab->coders[i].thread, NULL, coder_main,
				&lab->coders[i]))
		{
			pthread_mutex_lock(&lab->sched_mutex);
			lab->stopped = 1;
			wake_everyone(lab);
			pthread_mutex_unlock(&lab->sched_mutex);
			j = 0;
			while (j < i)
				pthread_join(lab->coders[j++].thread, NULL);
			pthread_join(lab->monitor, NULL);
			return (-1);
		}
		i++;
	}
	return (0);
}

int	lab_start(t_lab *lab)
{
	lab->dongles = calloc((size_t)lab->coders_n, sizeof(t_dongle));
	lab->coders = calloc((size_t)lab->coders_n, sizeof(t_coder));
	if (!lab->dongles || !lab->coders)
	{
		free(lab->dongles);
		free(lab->coders);
		return (-1);
	}
	queue_init(&lab->queue, lab->coders_n, lab->scheduler);
	if (!lab->queue.items || init_sync(lab))
	{
		queue_destroy(&lab->queue);
		free(lab->dongles);
		free(lab->coders);
		return (-1);
	}
	lab->stopped = 0;
	lab->next_arrival = 0;
	lab->start_time = wall_ms();
	if (spawn_threads(lab))
	{
		lab_destroy(lab);
		return (-1);
	}
	return (0);
}
