#include "codexion.h"

void	lab_join(t_lab *lab)
{
	int	i;

	pthread_join(lab->monitor, NULL);
	i = 0;
	while (i < lab->coders_n)
	{
		pthread_join(lab->coders[i].thread, NULL);
		i++;
	}
}

void	lab_destroy(t_lab *lab)
{
	int	i;

	i = 0;
	while (i < lab->coders_n)
	{
		pthread_cond_destroy(&lab->coders[i].cond);
		i++;
	}
	i = 0;
	while (i < lab->coders_n)
	{
		pthread_mutex_destroy(&lab->dongles[i].mutex);
		i++;
	}
	pthread_mutex_destroy(&lab->sched_mutex);
	pthread_mutex_destroy(&lab->log_mutex);
	queue_destroy(&lab->queue);
	free(lab->coders);
	free(lab->dongles);
}
