#include "codexion.h"

int	main(int argc, char **argv)
{
	t_lab	lab;

	memset(&lab, 0, sizeof(lab));
	if (parse_args(argc, argv, &lab))
		return (1);
	if (lab_start(&lab))
	{
		fprintf(stderr, "codexion: failed to initialize the simulation\n");
		return (1);
	}
	lab_join(&lab);
	lab_destroy(&lab);
	return (0);
}
