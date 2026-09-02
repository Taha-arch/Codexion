#include "codexion.h"

static int	precedes(t_queue *q, t_ticket *a, t_ticket *b)
{
	if (q->mode == ARBITRATION_EDF && a->deadline != b->deadline)
		return (a->deadline < b->deadline);
	return (a->arrival < b->arrival);
}

static void	swap_tickets(t_ticket *a, t_ticket *b)
{
	t_ticket	tmp;

	tmp = *a;
	*a = *b;
	*b = tmp;
}

static void	sift_up(t_queue *q, int i)
{
	int	parent;

	while (i > 0)
	{
		parent = (i - 1) / 2;
		if (!precedes(q, &q->items[i], &q->items[parent]))
			break ;
		swap_tickets(&q->items[i], &q->items[parent]);
		i = parent;
	}
}

static void	sift_down(t_queue *q, int i)
{
	int	left;
	int	right;
	int	best;

	while (1)
	{
		left = i * 2 + 1;
		right = i * 2 + 2;
		best = i;
		if (left < q->count && precedes(q, &q->items[left], &q->items[best]))
			best = left;
		if (right < q->count && precedes(q, &q->items[right], &q->items[best]))
			best = right;
		if (best == i)
			break ;
		swap_tickets(&q->items[i], &q->items[best]);
		i = best;
	}
}

void	queue_init(t_queue *q, int capacity, t_scheduler mode)
{
	q->items = malloc(sizeof(t_ticket) * (size_t)capacity);
	q->count = 0;
	q->capacity = capacity;
	q->mode = mode;
}

void	queue_destroy(t_queue *q)
{
	free(q->items);
	q->items = NULL;
}

int	queue_empty(t_queue *q)
{
	return (q->count == 0);
}

void	queue_push(t_queue *q, t_coder *coder, long arrival, long deadline)
{
	if (q->count == q->capacity)
		return ;
	q->items[q->count].coder = coder;
	q->items[q->count].arrival = arrival;
	q->items[q->count].deadline = deadline;
	sift_up(q, q->count);
	q->count++;
}

t_coder	*queue_peek(t_queue *q)
{
	if (q->count == 0)
		return (NULL);
	return (q->items[0].coder);
}

void	queue_pop(t_queue *q)
{
	if (q->count == 0)
		return ;
	q->count--;
	q->items[0] = q->items[q->count];
	sift_down(q, 0);
}
