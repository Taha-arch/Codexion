#include "codexion.h"

void	log_event(t_lab *lab, int coder_id, const char *msg)
{
	long	elapsed;

	elapsed = now_ms(lab);
	pthread_mutex_lock(&lab->log_mutex);
	printf("%ld %d %s\n", elapsed, coder_id, msg);
	pthread_mutex_unlock(&lab->log_mutex);
}
