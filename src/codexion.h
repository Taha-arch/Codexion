#ifndef CODEXION_H
# define CODEXION_H

# include <pthread.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/time.h>
# include <unistd.h>

# define POLL_INTERVAL_US 500

typedef enum e_scheduler
{
	ARBITRATION_FIFO,
	ARBITRATION_EDF
}	t_scheduler;

typedef struct s_dongle
{
	pthread_mutex_t	mutex;
	int				taken;
	long			free_at;
}	t_dongle;

typedef struct s_coder
{
	int				id;
	pthread_t		thread;
	pthread_cond_t	cond;
	int				ready;
	t_dongle		*left;
	t_dongle		*right;
	long			last_compile_start;
	int				compiles_done;
	struct s_lab	*lab;
}	t_coder;

typedef struct s_ticket
{
	t_coder	*coder;
	long	arrival;
	long	deadline;
}	t_ticket;

typedef struct s_queue
{
	t_ticket	*items;
	int			count;
	int			capacity;
	t_scheduler	mode;
}	t_queue;

typedef struct s_lab
{
	int				coders_n;
	long			burnout_ms;
	long			compile_ms;
	long			debug_ms;
	long			refactor_ms;
	int				compiles_required;
	long			cooldown_ms;
	t_scheduler		scheduler;
	long			start_time;
	int				stopped;
	long			next_arrival;
	t_queue			queue;
	pthread_mutex_t	sched_mutex;
	pthread_mutex_t	log_mutex;
	t_dongle		*dongles;
	t_coder			*coders;
	pthread_t		monitor;
}	t_lab;

/* args.c */
int		parse_args(int argc, char **argv, t_lab *lab);

/* clock_utils.c */
long	wall_ms(void);
long	now_ms(t_lab *lab);
void	wait_ms(t_coder *coder, long duration);

/* log.c */
void	log_event(t_lab *lab, int coder_id, const char *msg);

/* queue.c */
void	queue_init(t_queue *q, int capacity, t_scheduler mode);
void	queue_destroy(t_queue *q);
int		queue_empty(t_queue *q);
void	queue_push(t_queue *q, t_coder *coder, long arrival, long deadline);
t_coder	*queue_peek(t_queue *q);
void	queue_pop(t_queue *q);

/* dongle.c */
void	dongles_lock_pair(t_dongle *a, t_dongle *b);
void	dongles_unlock_pair(t_dongle *a, t_dongle *b);
int		dongles_try_take_pair(t_dongle *a, t_dongle *b, long now);
void	dongles_release_pair(t_dongle *a, t_dongle *b, long cooldown, long now);

/* scheduler.c */
void	request_dongles(t_coder *coder);
void	release_dongles(t_coder *coder);
void	try_grant(t_lab *lab);
void	wake_everyone(t_lab *lab);

/* coder.c */
void	*coder_main(void *arg);

/* monitor.c */
void	*monitor_main(void *arg);

/* lab_init.c */
int		lab_start(t_lab *lab);

/* lab_cleanup.c */
void	lab_join(t_lab *lab);
void	lab_destroy(t_lab *lab);

#endif
