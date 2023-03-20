CC = gcc
CFLAGS = -Wall -g -lpthread

source_path = /home/ubuntu/Projects/real_time_cyclic/src
out_path = /home/ubuntu/Projects/real_time_cyclic/out

.PHONY: all configurator master slave run clean

all: configurator master slave

configurator:
	$(CC) $(source_path)/configurator.c -o $(out_path)/configurator $(CFLAGS)

master:
	$(CC) $(source_path)/master.c -o $(out_path)/master $(CFLAGS)

slave:
	$(CC) $(source_path)/slave.c -o $(out_path)/slave $(CFLAGS)
	
run:
	# ./$(out_path)/configurator
	# valgrind --leak-check=full ./$(out_path)/master
	./$(out_path)/master
	# ./$(out_path)/slave

clean:
	rm -f $(out_path)/configurator $(out_path)/master $(out_path)/slave
