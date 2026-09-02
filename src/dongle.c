#include "codexion.h"

void	dongles_lock_pair(t_dongle *a, t_dongle *b)
{
	if (a == b)
	{
		pthread_mutex_lock(&a->mutex);
		return ;
	}
	if (a > b)
	{
		pthread_mutex_lock(&b->mutex);
		pthread_mutex_lock(&a->mutex);
	}
	else
	{
		pthread_mutex_lock(&a->mutex);
		pthread_mutex_lock(&b->mutex);
	}
}

void	dongles_unlock_pair(t_dongle *a, t_dongle *b)
{
	pthread_mutex_unlock(&a->mutex);
	if (a != b)
		pthread_mutex_unlock(&b->mutex);
}

static int	is_free(t_dongle *d, long now)
{
	return (!d->taken && now >= d->free_at);
}

int	dongles_try_take_pair(t_dongle *a, t_dongle *b, long now)
{
	int	ok;

	dongles_lock_pair(a, b);
	ok = is_free(a, now) && (a == b || is_free(b, now));
	if (ok)
	{
		a->taken = 1;
		b->taken = 1;
	}
	dongles_unlock_pair(a, b);
	return (ok);
}

void	dongles_release_pair(t_dongle *a, t_dongle *b, long cooldown, long now)
{
	dongles_lock_pair(a, b);
	a->taken = 0;
	a->free_at = now + cooldown;
	b->taken = 0;
	b->free_at = now + cooldown;
	dongles_unlock_pair(a, b);
}
