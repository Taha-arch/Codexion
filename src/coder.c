#include "codexion.h"

static int	is_stopped(t_lab *lab)
{
	int	stopped;

	pthread_mutex_lock(&lab->sched_mutex);
	stopped = lab->stopped;
	pthread_mutex_unlock(&lab->sched_mutex);
	return (stopped);
}

static int	start_compile(t_coder *coder)
{
	t_lab	*lab;

	lab = coder->lab;
	request_dongles(coder);
	if (!coder->ready)
		return (-1);
	log_event(lab, coder->id, "has taken a dongle");
	log_event(lab, coder->id, "has taken a dongle");
	pthread_mutex_lock(&lab->sched_mutex);
	coder->last_compile_start = now_ms(lab);
	pthread_mutex_unlock(&lab->sched_mutex);
	log_event(lab, coder->id, "is compiling");
	wait_ms(coder, lab->compile_ms);
	release_dongles(coder);
	return (0);
}

static void	debug_and_refactor(t_coder *coder)
{
	t_lab	*lab;

	lab = coder->lab;
	pthread_mutex_lock(&lab->sched_mutex);
	coder->compiles_done++;
	pthread_mutex_unlock(&lab->sched_mutex);
	log_event(lab, coder->id, "is debugging");
	wait_ms(coder, lab->debug_ms);
	if (is_stopped(lab))
		return ;
	log_event(lab, coder->id, "is refactoring");
	wait_ms(coder, lab->refactor_ms);
}

void	*coder_main(void *arg)
{
	t_coder	*coder;
	t_lab	*lab;

	coder = (t_coder *)arg;
	lab = coder->lab;
	while (!is_stopped(lab))
	{
		if (start_compile(coder))
			break ;
		if (is_stopped(lab))
			break ;
		debug_and_refactor(coder);
	}
	return (NULL);
}
