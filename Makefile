NAME		= codexion
CC			= cc
CFLAGS		= -Wall -Wextra -Werror -pthread
SRC_DIR		= src
HEADER		= $(SRC_DIR)/codexion.h

SRCS		= main.c args.c clock_utils.c log.c queue.c dongle.c \
			  scheduler.c coder.c monitor.c lab_init.c lab_cleanup.c
OBJS		= $(addprefix $(SRC_DIR)/, $(SRCS:.c=.o))

all: $(NAME)

$(NAME): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(NAME)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c $(HEADER)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all

.PHONY: all clean fclean re
