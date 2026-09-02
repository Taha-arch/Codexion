#include "codexion.h"

static int	parse_nonneg(const char *str, long *out)
{
	long	value;

	if (!str || !*str)
		return (-1);
	value = 0;
	while (*str)
	{
		if (*str < '0' || *str > '9')
			return (-1);
		value = value * 10 + (*str - '0');
		if (value > 1000000000)
			return (-1);
		str++;
	}
	*out = value;
	return (0);
}

static int	parse_scheduler(const char *str, t_scheduler *out)
{
	if (!strcmp(str, "fifo"))
		*out = ARBITRATION_FIFO;
	else if (!strcmp(str, "edf"))
		*out = ARBITRATION_EDF;
	else
		return (-1);
	return (0);
}

static void	print_usage(const char *prog)
{
	fprintf(stderr, "Usage: %s number_of_coders time_to_burnout "
		"time_to_compile time_to_debug time_to_refactor "
		"number_of_compiles_required dongle_cooldown scheduler\n", prog);
	fprintf(stderr, "  all numeric values must be non-negative integers "
		"(durations are in milliseconds)\n");
	fprintf(stderr, "  scheduler must be exactly \"fifo\" or \"edf\"\n");
}

static int	parse_numbers(char **argv, t_lab *lab)
{
	long	values[6];
	int		i;

	i = 0;
	while (i < 6)
	{
		if (parse_nonneg(argv[i + 2], &values[i]))
			return (-1);
		i++;
	}
	lab->burnout_ms = values[0];
	lab->compile_ms = values[1];
	lab->debug_ms = values[2];
	lab->refactor_ms = values[3];
	lab->compiles_required = (int)values[4];
	lab->cooldown_ms = values[5];
	return (0);
}

int	parse_args(int argc, char **argv, t_lab *lab)
{
	long	coders;

	if (argc != 9)
	{
		print_usage(argv[0]);
		return (-1);
	}
	if (parse_nonneg(argv[1], &coders) || coders == 0)
	{
		print_usage(argv[0]);
		return (-1);
	}
	lab->coders_n = (int)coders;
	if (parse_numbers(argv, lab) || parse_scheduler(argv[8], &lab->scheduler))
	{
		print_usage(argv[0]);
		return (-1);
	}
	return (0);
}
