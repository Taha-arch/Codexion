#include "codexion.h"

void	wake_everyone(t_lab *lab)
{
	int	i;

	i = 0;
	while (i < lab->coders_n)
	{
		pthread_cond_broadcast(&lab->coders[i].cond);
		i++;
	}
}

void	try_grant(t_lab *lab)
{
	t_coder	*coder;
	long	now;

	now = now_ms(lab);
	while (!queue_empty(&lab->queue))
	{
		coder = queue_peek(&lab->queue);
		if (!dongles_try_take_pair(coder->left, coder->right, now))
			break ;
		queue_pop(&lab->queue);
		coder->ready = 1;
		pthread_cond_signal(&coder->cond);
	}
}

void	request_dongles(t_coder *coder)
{
	t_lab	*lab;
	long	deadline;

	lab = coder->lab;
	pthread_mutex_lock(&lab->sched_mutex);
	coder->ready = 0;
	deadline = lab->burnout_ms;
	if (coder->last_compile_start >= 0)
		deadline += coder->last_compile_start;
	queue_push(&lab->queue, coder, lab->next_arrival++, deadline);
	while (!coder->ready && !lab->stopped)
		pthread_cond_wait(&coder->cond, &lab->sched_mutex);
	pthread_mutex_unlock(&lab->sched_mutex);
}

void	release_dongles(t_coder *coder)
{
	t_lab	*lab;

	lab = coder->lab;
	dongles_release_pair(coder->left, coder->right, lab->cooldown_ms,
		now_ms(lab));
}
