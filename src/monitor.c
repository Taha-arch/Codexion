#include "codexion.h"

static int	check_burnout(t_lab *lab)
{
	int		i;
	long	now;
	long	reference;

	now = now_ms(lab);
	i = 0;
	while (i < lab->coders_n)
	{
		reference = 0;
		if (lab->coders[i].last_compile_start >= 0)
			reference = lab->coders[i].last_compile_start;
		if (now - reference >= lab->burnout_ms)
		{
			lab->stopped = 1;
			log_event(lab, lab->coders[i].id, "burned out");
			return (1);
		}
		i++;
	}
	return (0);
}

static int	check_all_compiled(t_lab *lab)
{
	int	i;

	i = 0;
	while (i < lab->coders_n)
	{
		if (lab->coders[i].compiles_done < lab->compiles_required)
			return (0);
		i++;
	}
	lab->stopped = 1;
	return (1);
}

void	*monitor_main(void *arg)
{
	t_lab	*lab;
	int		done;

	lab = (t_lab *)arg;
	done = 0;
	while (!done)
	{
		pthread_mutex_lock(&lab->sched_mutex);
		done = check_burnout(lab) || check_all_compiled(lab);
		if (!done)
			try_grant(lab);
		else
			wake_everyone(lab);
		pthread_mutex_unlock(&lab->sched_mutex);
		if (!done)
			usleep(POLL_INTERVAL_US);
	}
	return (NULL);
}
